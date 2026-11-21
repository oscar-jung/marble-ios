/*
 * M3: GICv3 through the system-register CPU interface, on the boot core.
 *
 * Distributor: read ID registers, set ARE + Group 1 enables.
 * Redistributors: walk the frames (GICR_STRIDE apart) until GICR_TYPER.Last,
 * find the one whose affinity matches this core, wake it, put SGIs/PPIs in
 * Group 1 and enable the virtual timer (INTID 27), the EL1 physical timer
 * (INTID 30) and SGI 1. Then count interrupts in a 300 ms window for each
 * source; expected: ~30 timer interrupts at 100 Hz, 10 SGIs.
 *
 * On SM7475 the HLOS runs under Gunyah, so the distributor (and possibly the
 * redistributors) may be emulated by the hypervisor: what this page shows is
 * the interface XNU will see.
 */
#include "board.h"
#include "console.h"
#include "shim.h"

#define GICD_CTLR		0x0000
#define GICD_TYPER		0x0004
#define GICD_IIDR		0x0008
#define GICD_PIDR2		0xffe8

#define GICR_CTLR		0x0000
#define GICR_IIDR		0x0004
#define GICR_TYPER		0x0008
#define GICR_WAKER		0x0014
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ICPENDR0		(GICR_SGI + 0x280)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)

static volatile uint32_t irq_count, irq_other, irq_last, irq_src;
static uintptr_t my_gicr;

void gic_irq(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* next tick in 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static uint32_t my_affinity(void)
{
	uint64_t m = rsr("mpidr_el1");
	return (uint32_t)((m & 0xffffff) | ((m >> 8) & 0xff000000));	/* Aff3.Aff2.Aff1.Aff0 */
}

static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 32) & 0xff) << 48 |	/* Aff3 */
	       ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static void wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE + GICD_CTLR) & (1U << 31)); i++)
		;
}

/* Count interrupts from one source for 300 ms, after sampling the pending state with IRQs masked. */
static void window(const char *label, uint32_t src)
{
	uint64_t f = rsr("cntfrq_el0");

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	uint64_t t = ticks() + f / 50;	/* > one period, IRQs still masked */
	while (ticks() < t)
		;
	if (src == TEST_SGI) {
		wsr("icc_sgi1r_el1", sgi_self(TEST_SGI));
		isb();
	}
	uint32_t pend = rd32(my_gicr + GICR_ISPENDR0);
	uint64_t hppi = rsr("icc_hppir1_el1"), isr = rsr("isr_el1");
	uint64_t tctl = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = ticks() + f * 3 / 10;
	uint64_t next = ticks() + f / 100;
	for (int sent = 1; ticks() < t;)
		if (src == TEST_SGI && sent < 10 && ticks() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(TEST_SGI));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	if (src == PTIMER_INTID)
		wsr("cntp_ctl_el0", 0);
	isb();

	int ok = src == TEST_SGI ? irq_count == 10 : irq_count >= 20 && irq_count <= 40;
	con_color(ok ? C_GREEN : C_RED);
	con_printf("%-7s IRQS %u (EXPECT %s) OTHER %u LAST %u", label, irq_count,
		   src == TEST_SGI ? "10" : "~30", irq_other, irq_last);
	con_color(C_WHITE);
	con_printf("        PRE: PEND %08X HPPI %lX ISR %lX TCTL %lX", pend, hppi, isr, tctl);
}

void page_gic(void)
{
	stage = "gic dist id";
	uint32_t ctlr = rd32(GICD_BASE + GICD_CTLR), typer = rd32(GICD_BASE + GICD_TYPER);
	con_printf("GICD %08lX CTLR %08X TYPER %08X", GICD_BASE, ctlr, typer);
	con_printf("     IIDR %08X PIDR2 %08X (ARCHREV %u)", rd32(GICD_BASE + GICD_IIDR),
		   rd32(GICD_BASE + GICD_PIDR2), (rd32(GICD_BASE + GICD_PIDR2) >> 4) & 0xf);
	con_printf("     SPIS %u  SECURITY %s  LPIS %s", 32 * ((typer & 0x1f) + 1) - 32,
		   (typer >> 10) & 1 ? "2" : "1", (typer >> 17) & 1 ? "YES" : "NO");

	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	con_printf("ICC_SRE_EL1 %lX -> %lX", sre, rsr("icc_sre_el1"));
	if (!(rsr("icc_sre_el1") & 1)) {
		con_color(C_RED);
		con_printf("NO SYSTEM-REGISTER INTERFACE: STOP");
		return;
	}

	stage = "gic redist walk";
	uint32_t want = my_affinity();
	my_gicr = 0;
	con_printf("REDISTRIBUTORS (MINE: AFF %08X):", want);
	for (int i = 0; i < GICR_COUNT; i++) {
		uintptr_t r = GICR_BASE + (uintptr_t)i * GICR_STRIDE;
		uint64_t t = rd64(r + GICR_TYPER);
		uint32_t aff = (uint32_t)(t >> 32);
		con_printf(" %d %08lX AFF %08X PROC %3lu%s%s%s", i, r, aff, (t >> 8) & 0xffff,
			   t & (1 << 5) ? " DPGS" : "", t & (1 << 4) ? " LAST" : "", aff == want ? " <=" : "");
		if (aff == want)
			my_gicr = r;
		if (t & (1 << 4))
			break;
	}
	if (!my_gicr) {
		con_color(C_RED);
		con_printf("NO REDISTRIBUTOR FOR THIS CORE: STOP");
		return;
	}

	stage = "gic dist enable";
	wr32(GICD_BASE + GICD_CTLR, ctlr | 0x13);	/* ARE_NS | EnableGrp1A | EnableGrp1 (NS view) */
	wait_rwp();
	con_printf("GICD_CTLR %08X -> %08X", ctlr, rd32(GICD_BASE + GICD_CTLR));

	stage = "gic redist wake";
	uint32_t waker = rd32(my_gicr + GICR_WAKER);
	if (waker & 2) {
		wr32(my_gicr + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(my_gicr + GICR_WAKER) & 4); i++)
			;
	}
	con_printf("GICR_WAKER %08X -> %08X", waker, rd32(my_gicr + GICR_WAKER));

	stage = "gic redist config";
	uint32_t grp = rd32(my_gicr + GICR_IGROUPR0);
	wr32(my_gicr + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id++)
		wr8(my_gicr + GICR_IPRIORITYR + id, 0xa0);
	wr32(my_gicr + GICR_ICENABLER0, 0xffffffff);
	wr32(my_gicr + GICR_ICPENDR0, 0xffffffff);
	wr32(my_gicr + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << TEST_SGI));
	con_printf("IGROUPR0 %08X -> %08X  ISENABLER0 %08X", grp, rd32(my_gicr + GICR_IGROUPR0),
		   rd32(my_gicr + GICR_ISENABLER0));

	stage = "gic cpu iface";
	uint64_t ictlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ictlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_printf("ICC_CTLR %lX PMR %lX GRPEN1 %lX", ictlr, rsr("icc_pmr_el1"), rsr("icc_igrpen1_el1"));
	con_skip();

	stage = "gic vtimer";
	window("VTIMER", VTIMER_INTID);
	stage = "gic sgi";
	window("SGI1", TEST_SGI);
	con_skip();
	con_printf("EL1 PHYS TIMER (INTID %u) RUNS AFTER THIS PAGE", PTIMER_INTID);
	con_printf("HAS BEEN UP: IF IT PANICS, THE ABOVE STILL STANDS.");
}

void gic_ptimer_test(void)
{
	if (!my_gicr)
		return;
#if PTIMER_TRAPS
	con_color(C_YELLOW);
	con_printf("PTIMER  SKIPPED: GUNYAH TRAPS CNTP_* (UNDEF, M3)");
	con_color(C_WHITE);
	gic_quiesce();
	return;
#endif
	stage = "gic ptimer";
	window("PTIMER", PTIMER_INTID);
	wr32(my_gicr + GICR_ICENABLER0, 0xffffffff);
	wsr("icc_igrpen1_el1", 0);
	isb();
}

void gic_quiesce(void)
{
	__asm__ volatile("msr daifset, #0xf" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	if (my_gicr)
		wr32(my_gicr + GICR_ICENABLER0, 0xffffffff);
	if (rsr("icc_sre_el1") & 1)
		wsr("icc_igrpen1_el1", 0);
	isb();
}
