/*
 * marble-shim bring-up pages (docs/shim-design.md section 8, M0-M3).
 *
 * Each page clears the screen, runs its checks, draws the results as it
 * goes and stays up for PAGE_S seconds so it can be photographed:
 *   0  M0  summary: EL, FDT, CPU features, PSCI
 *   1  M1  FDT header, every /memory bank, initrd, bootargs text
 *   2  M1  every /reserved-memory child: base, size, no-map, dynamic
 *   3  M2  PSCI CPU_ON for every /cpus/cpu@ node, ID registers per core
 *   4  M3  GICv3 distributor and redistributors, vtimer/SGI interrupts,
 *          then (after the page has been up) the EL1 physical timer
 *          (skipped where it is known to trap: PTIMER_TRAPS)
 *   5  M5  XNU handoff: if the initrd holds a payload (MARBLXNU/SWEETXNU),
 *          check it, copy it to phys_base, write boot_args and jump
 * With a payload present the shim goes straight to page 5; shim.diag=1
 * runs pages 0-4 first. Without one it runs pages 0-4 and then resets
 * through PSCI (hold Volume Down for fastboot), or stays put with
 * shim.hold=1.
 *
 * Boot image cmdline options:
 *   shim.hold=1    never reset; stay on the last page
 *   shim.page=N    run only page N (0-4, or 6: read-only UFS probe), then hold
 *   shim.diag=1    with a payload: run pages 0-4 before the handoff
 *   shim.mdp=1     (page 0) read the MDP source pipes to find the scanout
 *                  surface; can hang the bus if MDP clocks are gated
 *   shim.fault=1   take a data abort after page 0 (tests the panic screen)
 * The older m0.* spellings are accepted too.
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "fdt.h"
#include "shim.h"

#define NUM_PAGES	6	/* 0-4 diagnostics, 5 handoff */

const char *volatile stage;	/* volatile: the exception handler reads it; run-time pointer only */
static uint32_t current_el;
static struct fb fb;
static uintptr_t args;
static uint32_t args_len;

/* ---- small services shared with smp.c / gic.c ---- */

uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
#if PSCI_CONDUIT_SMC
	__asm__ volatile("smc #0"
#else
	__asm__ volatile("hvc #0"
#endif
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

uint64_t ticks(void)
{
	isb();
	return rsr("cntpct_el0");
}

void delay_ms(uint64_t ms)
{
	uint64_t end = ticks() + rsr("cntfrq_el0") / 1000 * ms;

	while (ticks() < end)
		;
}

const char *cpu_part_name(uint64_t midr)
{
	uint32_t impl = (midr >> 24) & 0xff, part = (midr >> 4) & 0xfff;

	if (impl != 0x41)
		return impl == 0x51 ? "QCOM" : "?";
	switch (part) {
	case 0xd05: return "A55";
	case 0xd0b: return "A76";
	case 0xd0d: return "A77";
	case 0xd41: return "A78";
	case 0xd44: return "X1";
	case 0xd46: return "A510";
	case 0xd47: return "A710";
	case 0xd48: return "X2";
	case 0xd4d: return "A715";
	case 0xd4e: return "X3";
	default: return "?";
	}
}

/* ---- cmdline ---- */

static int arg_flag(const char *name)
{
	return args && mem_contains(args, args_len, name);
}

/* Single-digit value of "key=N" in bootargs, or -1. */
static int arg_digit(const char *key)
{
	uint32_t n = 0;

	while (key[n])
		n++;
	for (uint32_t i = 0; args && i + n < args_len; i++) {
		uint32_t j = 0;
		while (j < n && *(volatile char *)(args + i + j) == key[j])
			j++;
		if (j == n) {
			char c = *(volatile char *)(args + i + n);
			return c >= '0' && c <= '9' ? c - '0' : -1;
		}
	}
	return -1;
}

static int hold(void)
{
	return arg_flag("shim.hold=1") || arg_flag("m0.hold=1");
}

/* ---- ending ---- */

static void __attribute__((noreturn)) finish(int stay)
{
	con_color(C_YELLOW);
	if (stay) {
		con_printf("HOLD: LONG-PRESS POWER TO EXIT");
		for (;;)
			__asm__ volatile("wfe");
	}
	con_printf("RESET IN %u S - HOLD VOL- FOR FASTBOOT", RESET_DELAY_S);
	delay_ms((uint64_t)RESET_DELAY_S * 1000);
	dsb();
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	con_printf("PSCI RESET RETURNED!");
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	/* Names inline in a char array: a table of string pointers would be absolute relocations. */
	static const char kind[4][8] = { "SYNC", "IRQ", "FIQ", "SERROR" };

	if ((slot & 3) == 1) {	/* IRQ: handled and returned from */
		gic_irq();
		return;
	}
	con_scale(4);
	con_fill(C_DARKRED);
	con_color(C_WHITE);
	con_kv("PANIC", kind[slot & 3]);
	con_hex("SLOT", slot);
	con_kv("STAGE", stage);
	con_hex("MPIDR", rsr("mpidr_el1"));
	if (current_el == 2) {
		con_hex("ESR", rsr("esr_el2"));
		con_hex("ELR", rsr("elr_el2"));
		con_hex("FAR", rsr("far_el2"));
		con_hex("SPSR", rsr("spsr_el2"));
	} else {
		con_hex("ESR", rsr("esr_el1"));
		con_hex("ELR", rsr("elr_el1"));
		con_hex("FAR", rsr("far_el1"));
		con_hex("SPSR", rsr("spsr_el1"));
	}
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("LR", regs[31]);	/* x30 as stashed by the vector stub */
	con_skip();
	finish(hold());
}

/* ---- page frame ---- */

static void page_begin(int n, const char *title, uint32_t scale)
{
	con_scale(scale);
	con_fill(C_NAVY);
	con_bars();
	con_color(C_YELLOW);
	con_printf("MARBLE SHIM  PAGE %d/%d  %s", n, NUM_PAGES - 1, title);
	con_color(C_WHITE);
}

static void page_end(void)
{
	if (con_dropped()) {
		con_color(C_RED);
		con_printf("(%u LINES DID NOT FIT)", con_dropped());
		con_color(C_WHITE);
	}
}

/* ---- page 0: M0 summary ---- */

#ifdef MDP_BASE
#define SSPP_SRC_SIZE		0x00	/* h << 16 | w */
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24	/* low 16 bits: plane 0 pitch */
#define SSPP_SRC_FORMAT		0x30	/* bits 10:9 = bytes per pixel - 1 */

static int mdp_probe(struct fb *out)
{
	static const uint32_t off[MDP_NUM_SSPP] = MDP_SSPP_OFFSETS;
	int found = 0;

	stage = "mdp probe";
	con_printf("MDP SOURCE PIPES:");
	for (int i = 0; i < MDP_NUM_SSPP; i++) {
		uintptr_t b = MDP_BASE + off[i];
		uint32_t size = rd32(b + SSPP_SRC_SIZE);
		uint32_t addr = rd32(b + SSPP_SRC0_ADDR);
		uint32_t stride = rd32(b + SSPP_SRC_YSTRIDE0) & 0xffff;
		uint32_t fmt = rd32(b + SSPP_SRC_FORMAT);

		con_printf("SSPP%d A=%08X S=%08X P=%04X F=%08X", i, addr, size, stride, fmt);
		uint32_t w = size & 0xffff, h = size >> 16, bpp = ((fmt >> 9) & 3) + 1;
		if (!found && addr >= FB_DEF_BASE && addr < FB_DEF_BASE + FB_REGION_SIZE &&
		    bpp == 4 && w && h && stride >= w * 4 &&
		    (uint64_t)stride * h <= FB_DEF_BASE + FB_REGION_SIZE - addr) {
			out->base = addr;
			out->width = w;
			out->height = h;
			out->stride = stride;
			found = 1;
		}
	}
	return found;
}
#endif

static const char *yes_no(uint64_t v)
{
	return v ? "YES" : "NO";
}

static void page_m0(uintptr_t fdt, uintptr_t image_base)
{
	stage = "page 0";
	page_begin(0, "M0", 4);

#ifdef MDP_BASE
	if (arg_flag("shim.mdp=1") || arg_flag("m0.mdp=1")) {
		struct fb probed = fb;
		if (mdp_probe(&probed) &&
		    (probed.base != fb.base || probed.stride != fb.stride || probed.width != fb.width)) {
			delay_ms(5000);	/* time to read the pipe table */
			fb = probed;
			con_init(&fb, C_NAVY, C_WHITE);
			page_begin(0, "M0 (FB FROM MDP)", 4);
		}
	}
#endif
	con_printf("%s", BOARD_NAME);
	con_printf("EL %u  IMAGE %010lX", current_el, image_base);
	con_printf("FB %08lX P=%u", fb.base, fb.stride);
	con_color(fdt_valid(fdt) ? C_GREEN : C_RED);
	con_printf("FDT %08lX N=%X", fdt, fdt_totalsize(fdt));
	con_color(C_WHITE);

	uint64_t midr = rsr("midr_el1"), isar1 = rsr("id_aa64isar1_el1");
	uint64_t isar2 = rsr("s3_0_c0_c6_2");	/* ID_AA64ISAR2_EL1 */
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_printf("MIDR %08lX %s", midr, cpu_part_name(midr));
	con_printf("MPIDR %08lX", rsr("mpidr_el1"));
	int pac = FIELD(isar1, 4) || FIELD(isar1, 8) || FIELD(isar2, 12);
	con_color(pac ? C_GREEN : C_RED);
	con_printf("PAC %s APA=%lu API=%lu APA3=%lu GPA=%lu", yes_no(pac), FIELD(isar1, 4),
		   FIELD(isar1, 8), FIELD(isar2, 12), FIELD(isar1, 24));
	con_color(FIELD(mmfr0, 20) ? C_GREEN : C_RED);
	con_printf("16K %s TGRAN16=%lu PA=%lu", yes_no(FIELD(mmfr0, 20)), FIELD(mmfr0, 20),
		   FIELD(mmfr0, 0));
	con_color(C_WHITE);
	con_printf("CNTFRQ %lu", rsr("cntfrq_el0"));
	stage = "psci";
	con_printf("PSCI %X SMCCC %X", (uint32_t)psci(PSCI_VERSION, 0, 0, 0), (uint32_t)psci(SMCCC_VERSION, 0, 0, 0));
	con_printf("RESET2 FEATURES %X (0 = SUPPORTED)", (uint32_t)psci(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0));
	page_end();
}

/* ---- page 1: memory and /chosen ---- */

static void page_memory(uintptr_t fdt)
{
	stage = "page 1";
	page_begin(1, "M1 MEMORY", 3);
	if (!fdt_valid(fdt)) {
		con_printf("NO VALID FDT AT %lX", fdt);
		return;
	}
	con_printf("FDT %08lX SIZE %X VER %u BOOTCPU %u", fdt, fdt_totalsize(fdt), fdt_version(fdt),
		   fdt_boot_cpuid(fdt));
	con_skip();

	uint32_t mlen = 0;
	uintptr_t mreg = fdt_prop(fdt, "memory", "reg", &mlen);
	uint64_t total = 0;
	con_printf("/MEMORY REG: %u BYTES (2+2 CELLS ASSUMED)", mlen);
	for (uint32_t off = 0; mreg && off + 16 <= mlen; off += 16) {
		uint64_t base = (uint64_t)be32(mreg + off) << 32 | be32(mreg + off + 4);
		uint64_t size = (uint64_t)be32(mreg + off + 8) << 32 | be32(mreg + off + 12);
		total += size;
		con_printf(" %u: %010lX - %010lX  %5lu MIB", off / 16, base, base + size, size >> 20);
	}
	con_printf(" TOTAL %lu MIB", total >> 20);
	con_skip();

	uint64_t lo = 0, hi = 0;
	if (fdt_prop_u64(fdt, "chosen", "linux,initrd-start", &lo) &&
	    fdt_prop_u64(fdt, "chosen", "linux,initrd-end", &hi))
		con_printf("INITRD %010lX - %010lX  %lu KIB", lo, hi, (hi - lo) >> 10);
	uint32_t l = 0;
	con_printf("KASLR-SEED %s  RNG-SEED %s",
		   yes_no(fdt_prop(fdt, "chosen", "kaslr-seed", &l)),
		   yes_no(fdt_prop(fdt, "chosen", "rng-seed", &l)));
	con_skip();
	con_printf("BOOTARGS (%u BYTES):", args_len);
	if (args)
		con_wrap(args, args_len, 1);
	page_end();
}

/* ---- page 2: reserved-memory ---- */

struct resv_ctx {
	uint64_t static_bytes, dynamic_bytes;
};

static void resv_line(void *ctxp, int i, const struct fdt_child *c)
{
	struct resv_ctx *ctx = ctxp;
	char name[23];
	int n = 0;

	for (; n < 22 && *(volatile char *)(c->name + n); n++)
		name[n] = *(volatile char *)(c->name + n);
	name[n] = 0;
	if (c->reg && c->reg_len >= 16) {
		uint64_t base = (uint64_t)be32(c->reg) << 32 | be32(c->reg + 4);
		uint64_t size = (uint64_t)be32(c->reg + 8) << 32 | be32(c->reg + 12);
		ctx->static_bytes += size;
		con_printf("%2d %-22s %010lX %8lX%s%s", i, name, base, size, c->no_map ? " N" : "",
			   c->reg_len > 16 ? " +" : "");
	} else if (c->size && c->size_len >= 4) {
		uint64_t size = c->size_len >= 8 ? (uint64_t)be32(c->size) << 32 | be32(c->size + 4) : be32(c->size);
		ctx->dynamic_bytes += size;
		con_printf("%2d %-22s DYNAMIC    %8lX%s", i, name, size, c->no_map ? " N" : "");
	} else {
		con_printf("%2d %-22s (NO REG/SIZE)", i, name);
	}
}

static void page_reserved(uintptr_t fdt)
{
	struct resv_ctx ctx = { 0, 0 };

	stage = "page 2";
	page_begin(2, "M1 RESERVED-MEMORY", 3);
	con_printf("#  NAME                   BASE       SIZE  N=NO-MAP");
	int n = fdt_foreach_child(fdt, "reserved-memory", resv_line, &ctx);
	con_printf("%d NODES  STATIC %lu MIB  DYNAMIC %lu MIB", n, ctx.static_bytes >> 20, ctx.dynamic_bytes >> 20);
	page_end();
}

/* ---- main ---- */

static void run_page(int n, uintptr_t fdt, uintptr_t image_base)
{
	switch (n) {
	case 0:
		page_m0(fdt, image_base);
		break;
	case 1:
		page_memory(fdt);
		break;
	case 2:
		page_reserved(fdt);
		break;
	case 3:
		page_begin(3, "M2 SMP", 3);
		page_smp(fdt);
		page_end();
		break;
	case 4:
		page_begin(4, "M3 GIC + TIMER", 3);
		page_gic();
		page_end();
		break;
	case 6:
		page_begin(6, "UFS READ-ONLY PROBE", 3);
		page_ufs();
		page_end();
		break;
	}
}

void shim_main(uintptr_t fdt, uint64_t el, uintptr_t image_base)
{
	fb.base = FB_DEF_BASE;
	fb.width = FB_DEF_W;
	fb.height = FB_DEF_H;
	fb.stride = FB_DEF_STRIDE;
	current_el = (uint32_t)el;
	stage = "start";

	/* First pixels before touching anything else: proof that we run. */
	con_init(&fb, C_NAVY, C_WHITE);
	con_fill(C_NAVY);
	con_bars();

	stage = "fdt";
	args = fdt_prop(fdt, "chosen", "bootargs", &args_len);

	int only = arg_digit("shim.page=");
	uint64_t rd_lo = 0, rd_hi = 0;
	uintptr_t payload = 0;
	if (fdt_prop_u64(fdt, "chosen", "linux,initrd-start", &rd_lo) &&
	    fdt_prop_u64(fdt, "chosen", "linux,initrd-end", &rd_hi) && rd_hi > rd_lo) {
		stage = "payload scan";
		payload = payload_find(rd_lo, rd_hi);
	}

	if ((only >= 0 && only < NUM_PAGES - 1) || only == 6) {
		run_page(only, fdt, image_base);
		if (only == 4) {
			delay_ms((uint64_t)PAGE_S * 1000);
			gic_ptimer_test();
		}
		finish(1);
	}

	if (!payload || arg_flag("shim.diag=1")) {
		for (int n = 0; n < NUM_PAGES - 1; n++) {
			run_page(n, fdt, image_base);
			if (n == 0 && (arg_flag("shim.fault=1") || arg_flag("m0.fault=1"))) {
				stage = "fault test";
				(void)*(volatile uint32_t *)0xfffffffffff0UL;
			}
			delay_ms((uint64_t)PAGE_S * 1000);
		}
		/* Riskiest check last, on a page that has already been up for PAGE_S. */
		gic_ptimer_test();
	}

	if (payload) {
		page_begin(5, "M5 XNU HANDOFF", 3);
		loader_page(fdt, &fb, payload, rd_lo, rd_hi, LOADER_WAIT_S);
		page_end();
		finish(1);	/* only reached when the payload was rejected */
	}
	finish(hold());
}
