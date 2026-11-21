/*
 * USB log channel: the shim as a minimal USB device (DWC3, device mode) with
 * one vendor-specific interface and one bulk IN endpoint that streams
 * everything the console prints. Host side: tools/usblog.py.
 *
 * Adapted from ios-sweet's XNU driver bsd/dev/sweet_usb.c (MIT, dipa-studio),
 * which follows the Synopsys DWC3 databook as used by Linux drivers/usb/dwc3.
 * Like it, this relies on ABL: fastboot just ran on this controller, so the
 * power domain, clocks and the PHY (marble: eUSB2 + repeater) are left up.
 * Only the device controller is restarted: soft reset, an event buffer in our
 * memory, endpoint 0, VBUS-valid override in the Qualcomm wrapper, run.
 *
 * Polled, no interrupts. With the MMU off every buffer is Device memory, so
 * DMA needs no cache maintenance. All register accesses are single ldr/str.
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "shim.h"

#ifdef USB_DWC3_BASE


#define GCTL		0xc110
#define GSNPSID		0xc120
#define GEVNTADRLO	0xc400
#define GEVNTADRHI	0xc404
#define GEVNTSIZ	0xc408
#define GEVNTCOUNT	0xc40c
#define DCFG		0xc700
#define DCTL		0xc704
#define DEVTEN		0xc708
#define DSTS		0xc70c
#define DALEPENA	0xc720
#define DEPCMDPAR2(n)	(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)	(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)	(0xc808 + 0x10 * (n))
#define DEPCMD(n)	(0xc80c + 0x10 * (n))

#define QS_HS_PHY_CTRL	0x10
#define   UTMI_OTG_VBUS_VALID	(1u << 20)
#define   SW_SESSVLD_SEL	(1u << 28)
#define QS_SS_PHY_CTRL	0x30
#define   LANE0_PWR_PRESENT	(1u << 24)

#define DCTL_RUN_STOP	(1u << 31)
#define DCTL_CSFTRST	(1u << 30)
#define DSTS_DEVCTRLHLT	(1u << 22)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_CMDACT		(1u << 10)

#define TRB_HWO		(1u << 0)
#define TRB_LST		(1u << 1)
#define TRB_ISP_IMI	(1u << 10)
#define TRB_IOC		(1u << 11)
#define TRBCTL_NORMAL	(1u << 4)
#define TRBCTL_SETUP	(2u << 4)
#define TRBCTL_STATUS2	(3u << 4)
#define TRBCTL_STATUS3	(4u << 4)
#define TRBCTL_DATA	(5u << 4)

#define EP_TX		3		/* physical 3 = 0x81 bulk IN */
#define EVT_SIZE	0x1000
#define TX_MAX		16383		/* never a multiple of 512: every transfer ends short */
#define LOG_SIZE	(512u << 10)

static inline uint32_t rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]; dsb sy" : : "r"(v), "r"(a) : "memory");
}

#define DWC(off)	(USB_DWC3_BASE + (off))

static struct {
	uint32_t trb[4];
} trb_out __attribute__((aligned(16))), trb_in __attribute__((aligned(16))), trb_tx __attribute__((aligned(16)));
static uint32_t evt[EVT_SIZE / 4] __attribute__((aligned(4096)));
static uint8_t setup_pkt[8] __attribute__((aligned(64)));
static uint8_t ep0_data[512] __attribute__((aligned(64)));
static uint8_t tx_buf[TX_MAX + 1] __attribute__((aligned(4096)));
static char log_ring[LOG_SIZE];

static struct {
	int up;			/* controller running */
	int configured;
	int tx_busy;
	uint32_t tx_len;
	uint32_t evt_pos;
	uint64_t log_head;	/* bytes ever logged */
	uint64_t log_sent;	/* bytes handed to the host */
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	uint32_t events, resets, setups;
	const char *fail;
	int reboot;		/* host asked for a reset: done after the status stage */
} u;

/* VID 0x1209 / PID 0x0001: pid.codes test ID (not a product), as ios-sweet. */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0xff, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x01, 0x00, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[25] = {
	9, 2, 25, 0, 1, 1, 0, 0x80, 250,	/* 1 interface, 500 mA */
	9, 4, 0, 0, 1, 0xff, 0x00, 0x00, 0,	/* interface 0: vendor, 1 endpoint */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,	/* ep 0x81 bulk IN, 512 */
};
static const char str_mfr[] = "marble-ios";
static const char str_prod[] = "marble shim log";
static const char str_ser[] = "0001";

static void copy(void *d, const void *s, uint32_t n)
{
	volatile uint8_t *dp = d;
	const volatile uint8_t *sp = s;

	while (n--)
		*dp++ = *sp++;
}

static int epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	wr(DWC(DEPCMDPAR0(ep)), p0);
	wr(DWC(DEPCMDPAR1(ep)), p1);
	wr(DWC(DEPCMDPAR2(ep)), p2);
	wr(DWC(DEPCMD(ep)), cmd | DEPCMD_CMDACT);
	uint64_t end = ticks() + rsr("cntfrq_el0") / 100;	/* 10 ms */
	while (ticks() < end) {
		uint32_t v = rd(DWC(DEPCMD(ep)));
		if (!(v & DEPCMD_CMDACT))
			return (v >> 12) & 0xf ? -1 : 0;
	}
	return -1;
}

static void start_trb(unsigned ep, void *trbp, const void *buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = trbp;
	uintptr_t b = (uintptr_t)buf, tp = (uintptr_t)trbp;

	t[0] = (uint32_t)b;
	t[1] = (uint32_t)(b >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	dsb();
	epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tp >> 32), (uint32_t)tp, 0);
}

static void start_setup(void)
{
	u.state = EP0_SETUP;
	start_trb(0, &trb_out, setup_pkt, 8, TRBCTL_SETUP);
}

static uint32_t string_desc(unsigned idx, uint8_t *out)
{
	const char *s = idx == 1 ? str_mfr : idx == 2 ? str_prod : idx == 3 ? str_ser : 0;
	uint32_t n = 2;

	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;	/* en-US */
		return 4;
	}
	if (!s)
		return 0;
	for (; *s && n < 126; s++) {
		out[n++] = (uint8_t)*s;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void tx_kick(void)
{
	if (!u.configured || u.tx_busy || u.log_sent == u.log_head)
		return;
	if (u.log_head - u.log_sent > LOG_SIZE)	/* overrun: skip what was overwritten */
		u.log_sent = u.log_head - LOG_SIZE;
	uint32_t n = 0;
	while (n < TX_MAX && u.log_sent + n < u.log_head) {
		tx_buf[n] = (uint8_t)log_ring[(u.log_sent + n) % LOG_SIZE];
		n++;
	}
	if (n && (n % 512) == 0)
		n--;			/* keep every transfer short-terminated */
	if (!n)
		return;
	u.tx_len = n;
	u.tx_busy = 1;
	start_trb(EP_TX, &trb_tx, tx_buf, n, TRBCTL_NORMAL);
}

static void configure(void)
{
	if (u.configured)
		return;
	if (epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		u.fail = "bulk endpoint setup";
		return;
	}
	wr(DWC(DALEPENA), rd(DWC(DALEPENA)) | (1u << EP_TX));
	u.configured = 1;
	tx_kick();
}

static void setup(void)
{
	uint8_t type = setup_pkt[0], req = setup_pkt[1];
	uint16_t value = (uint16_t)(setup_pkt[2] | setup_pkt[3] << 8);
	uint16_t length = (uint16_t)(setup_pkt[6] | setup_pkt[7] << 8);
	uint32_t n = 0;
	int ok = 1, in = (type & 0x80) != 0;

	u.setups++;
	if (type == 0x40 && req == 'R' && length == 0) {	/* vendor: reboot (tools/usblog.py --reboot) */
		u.reboot = 1;
	} else if ((type & 0x60) != 0) {
		ok = 0;
	} else if (req == 6) {			/* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: copy(ep0_data, dev_desc, sizeof(dev_desc)); n = sizeof(dev_desc); break;
		case 2: copy(ep0_data, cfg_desc, sizeof(cfg_desc)); n = sizeof(cfg_desc); break;
		case 3: n = string_desc(value & 0xff, ep0_data); ok = n != 0; break;
		default: ok = 0; break;
		}
	} else if (req == 5) {			/* SET_ADDRESS */
		wr(DWC(DCFG), (rd(DWC(DCFG)) & ~(0x7fu << 3)) | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {			/* SET_CONFIGURATION */
		if (value == 1)
			configure();
	} else if (req == 8) {			/* GET_CONFIGURATION */
		ep0_data[0] = (uint8_t)u.configured;
		n = 1;
	} else if (req == 0) {			/* GET_STATUS */
		ep0_data[0] = ep0_data[1] = 0;
		n = 2;
	} else if (req == 10) {			/* GET_INTERFACE */
		ep0_data[0] = 0;
		n = 1;
	} else if (req == 11 || req == 1 || req == 3) {	/* SET_INTERFACE, CLEAR/SET_FEATURE */
	} else {
		ok = 0;
	}
	if (!ok) {
		epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
		start_setup();
		return;
	}
	if (length == 0) {
		u.state = EP0_WAIT_STATUS2;	/* status stage on XferNotReady */
		return;
	}
	u.state = EP0_DATA;
	if (in)
		start_trb(1, &trb_in, ep0_data, n > length ? length : n, TRBCTL_DATA);
	else
		start_trb(0, &trb_out, ep0_data, 64, TRBCTL_DATA);
}

static void event(uint32_t ev)
{
	if (ev & 1) {				/* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {		/* USB reset */
			u.resets++;
			wr(DWC(DCFG), rd(DWC(DCFG)) & ~(0x7fu << 3));
		} else if (type == 2) {		/* connect done: ep0 max packet 64 */
			epcmd(0, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
			epcmd(1, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
		} else if (type == 0) {		/* disconnect */
			u.configured = 0;
			u.tx_busy = 0;
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f, type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;

	if (ep == EP_TX && type == 1) {		/* bulk IN done */
		u.log_sent += u.tx_len;
		u.tx_busy = 0;
		tx_kick();
		return;
	}
	if (ep > 1)
		return;
	if (type == 1) {			/* XferComplete */
		if (u.state == EP0_SETUP)
			setup();
		else if (u.state == EP0_DATA)
			u.state = EP0_WAIT_STATUS3;
		else if (u.state == EP0_STATUS) {
			if (u.reboot) {
				/* Host-requested reset: Android boots (no reboot reason), the host's
				 * orchestrator then uses adb to get back to fastboot. */
				dsb();
				psci(PSCI_SYSTEM_RESET, 0, 0, 0);
			}
			start_setup();
		}
		return;
	}
	if (type == 3 && status == 2 &&		/* XferNotReady, control status phase */
	    (u.state == EP0_WAIT_STATUS2 || u.state == EP0_WAIT_STATUS3)) {
		uint32_t ctl = u.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
		u.state = EP0_STATUS;
		start_trb(ep, ep ? (void *)&trb_in : (void *)&trb_out, ep0_data, 0, ctl);
	}
}

/* ---- public ---- */

void usb_poll(void)
{
	if (!u.up)
		return;
	uint32_t count = rd(DWC(GEVNTCOUNT)) & 0xffff;
	for (uint32_t done = 0; done < count; done += 4) {
		uint32_t ev = evt[u.evt_pos / 4];
		u.evt_pos = (u.evt_pos + 4) % EVT_SIZE;
		u.events++;
		event(ev);
	}
	if (count)
		wr(DWC(GEVNTCOUNT), count);
	tx_kick();
}

/* Console tee: every character drawn on screen also goes to the host. */
void con_tee(char c)
{
	log_ring[u.log_head % LOG_SIZE] = c;
	u.log_head++;
}

int usb_configured(void)
{
	return u.configured;
}

/* Returns 0 when running; otherwise prints why on the console. */
int usb_start(void)
{
	/*
	 * No GCC power/clock check here: on marble a read of GCC_USB30_PRIM_GDSCR
	 * (0x10f004) from EL1 is a synchronous external abort (phone, 2026-11-21;
	 * GCC is not open to the HLOS here, unlike sweet). ABL drove fastboot on
	 * this controller until the handoff, so its power domain and clocks are on.
	 */
	stage = "usb dwc3 id";
	con_printf("USB: GSNPSID %08X GCTL %08X DCTL %08X DSTS %08X", rd(DWC(GSNPSID)), rd(DWC(GCTL)),
		   rd(DWC(DCTL)), rd(DWC(DSTS)));
	con_printf("USB: QSCRATCH HS %08X SS %08X", rd(USB_QSCRATCH_BASE + QS_HS_PHY_CTRL),
		   rd(USB_QSCRATCH_BASE + QS_SS_PHY_CTRL));

	stage = "usb soft reset";
	wr(DWC(DCTL), DCTL_CSFTRST);
	uint64_t end = ticks() + rsr("cntfrq_el0") / 2;
	while ((rd(DWC(DCTL)) & DCTL_CSFTRST) && ticks() < end)
		;
	if (rd(DWC(DCTL)) & DCTL_CSFTRST) {
		con_printf("USB: SOFT RESET STUCK");
		return -1;
	}

	stage = "usb setup";
	uintptr_t ea = (uintptr_t)evt;
	wr(DWC(GEVNTADRLO), (uint32_t)ea);
	wr(DWC(GEVNTADRHI), (uint32_t)(ea >> 32));
	wr(DWC(GEVNTSIZ), (1u << 31) | EVT_SIZE);	/* interrupt masked: polled */
	wr(DWC(GEVNTCOUNT), rd(DWC(GEVNTCOUNT)) & 0xffff);
	u.evt_pos = 0;
	wr(DWC(DCFG), rd(DWC(DCFG)) & ~(7u | (0x7fu << 3)));	/* high speed, address 0 */
	wr(DWC(DEVTEN), 0x7);				/* disconnect, reset, connect done */
	if (epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    epcmd(0, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10), 0) ||
	    epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(1, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		con_printf("USB: ENDPOINT 0 SETUP FAILED");
		return -1;
	}
	wr(DWC(DALEPENA), 0x3);
	start_setup();

	stage = "usb vbus override";
	wr(USB_QSCRATCH_BASE + QS_HS_PHY_CTRL, rd(USB_QSCRATCH_BASE + QS_HS_PHY_CTRL) | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	wr(USB_QSCRATCH_BASE + QS_SS_PHY_CTRL, rd(USB_QSCRATCH_BASE + QS_SS_PHY_CTRL) | LANE0_PWR_PRESENT);

	stage = "usb run";
	wr(DWC(DCTL), rd(DWC(DCTL)) | DCTL_RUN_STOP);
	end = ticks() + rsr("cntfrq_el0") / 2;
	while ((rd(DWC(DSTS)) & DSTS_DEVCTRLHLT) && ticks() < end)
		;
	u.up = 1;
	con_printf("USB: RUNNING, DSTS %08X. WAITING FOR HOST...", rd(DWC(DSTS)));
	return 0;
}

/* Wait (polling) until the host configured us, or ms elapsed. */
void usb_wait_host(uint32_t ms)
{
	uint64_t end = ticks() + rsr("cntfrq_el0") / 1000 * ms;

	while (u.up && !u.configured && ticks() < end)
		usb_poll();
	con_printf("USB: %s (EVENTS %u RESETS %u SETUPS %u)%s%s", u.configured ? "HOST CONNECTED" : "NO HOST YET",
		   u.events, u.resets, u.setups, u.fail ? " FAIL: " : "", u.fail ? u.fail : "");
}

/* Before handing the controller to the next stage: flush what we can, then stop. */
void usb_stop(void)
{
	if (!u.up)
		return;
	uint64_t end = ticks() + rsr("cntfrq_el0") / 2;
	while (u.configured && u.log_sent != u.log_head && ticks() < end)
		usb_poll();
	wr(DWC(DCTL), rd(DWC(DCTL)) & ~DCTL_RUN_STOP);
	u.up = 0;
}

#else
void usb_poll(void) {}
void con_tee(char c) { (void)c; }
int usb_start(void) { return -1; }
int usb_configured(void) { return 0; }
void usb_wait_host(uint32_t ms) { (void)ms; }
void usb_stop(void) {}
#endif
