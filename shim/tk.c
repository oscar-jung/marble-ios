/*
 * tk: a stand-in for XNU to test the shim's handoff (M5a).
 *
 * Packed by tools/mkpayload_tk.py exactly where a kernel would go, it is
 * entered the way XNU's _start is: MMU and caches off, x0 = boot_args
 * (physical). It shows what it was handed, checks the ADT copy, and then
 * writes and reads back a pattern across the whole RAM range it was given,
 * which proves Gunyah's stage 2 lets the HLOS touch all of it.
 *
 * Shares start.S (Image header, vectors), console.c and libc.c with the
 * shim; start.S calls shim_main(x0, CurrentEL, load address).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "payload.h"
#include "shim.h"

const char *volatile stage;
static uint32_t current_el;
static int hold;

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

static void __attribute__((noreturn)) finish(void)
{
	con_color(C_YELLOW);
	if (hold) {
		con_printf("TK HOLD: LONG-PRESS POWER TO EXIT");
		for (;;)
			__asm__ volatile("wfe");
	}
	con_printf("TK: RESET IN %u S - HOLD VOL- FOR FASTBOOT", RESET_DELAY_S);
	delay_ms((uint64_t)RESET_DELAY_S * 1000);
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_scale(4);
	con_fill(C_DARKRED);
	con_color(C_WHITE);
	con_printf("TK PANIC SLOT %lu", slot);
	con_printf("STAGE %s", stage);
	con_printf("ESR %016lX", rsr("esr_el1"));
	con_printf("ELR %016lX", rsr("elr_el1"));
	con_printf("FAR %016lX", rsr("far_el1"));
	con_printf("X0 %016lX", regs[0]);
	con_printf("LR %016lX", regs[31]);
	finish();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

static int contains(const volatile char *s, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len && s[i]; i++) {
		uint32_t j = 0;
		while (needle[j] && i + j < len && s[i + j] == needle[j])
			j++;
		if (!needle[j])
			return 1;
	}
	return 0;
}

/* RAM probe: one 64-bit word every `step` bytes over [lo, hi), skipping [skip_lo, skip_hi). */
static void ram_probe(uint64_t lo, uint64_t hi, uint64_t skip_lo, uint64_t skip_hi)
{
	uint64_t step = (hi - lo) / 64, ok = 0, bad = 0, first_bad = 0;

	step &= ~0xfffUL;
	if (!step)
		step = 0x1000;
	stage = "ram probe";
	for (uint64_t a = lo + 0x800; a < hi; a += step) {
		if (a >= skip_lo && a < skip_hi)
			continue;
		volatile uint64_t *w = (volatile uint64_t *)a;
		uint64_t want = a ^ 0x5a5aa5a5c3c33c3cUL;
		*w = want;
		__asm__ volatile("dsb sy" ::: "memory");
		if (*w == want) {
			ok++;
		} else {
			if (!bad)
				first_bad = a;
			bad++;
		}
	}
	con_color(bad ? C_RED : C_GREEN);
	con_printf("RAM PROBE %010lX-%010lX: %lu OK %lu BAD", lo, hi, ok, bad);
	if (bad)
		con_printf("  FIRST BAD %010lX", first_bad);
	con_color(C_WHITE);
}

void shim_main(uintptr_t x0, uint64_t el, uintptr_t image_base)
{
	volatile struct boot_args *ba = (volatile struct boot_args *)x0;
	struct fb fb;

	current_el = (uint32_t)el;
	stage = "tk start";
	fb.base = ba->Video.v_baseAddr;
	fb.width = (uint32_t)ba->Video.v_width;
	fb.height = (uint32_t)ba->Video.v_height;
	fb.stride = (uint32_t)ba->Video.v_rowBytes;
	if (!fb.base || !fb.width || !fb.height || fb.stride < fb.width * 4) {
		fb.base = FB_DEF_BASE;
		fb.width = FB_DEF_W;
		fb.height = FB_DEF_H;
		fb.stride = FB_DEF_STRIDE;
	}
	con_init(&fb, C_BLACK, C_WHITE);
	con_scale(3);
	con_fill(0xff003000);	/* dark green: obviously not the shim's navy */
	con_bars();
	hold = contains(ba->CommandLine, BOOT_LINE_LENGTH, "tk.hold=1");

	con_color(C_YELLOW);
	con_printf("TK: FAKE XNU RECEIVED CONTROL");
	con_color(C_WHITE);
	con_printf("EL %u  LOADED AT %010lX  X0 %010lX", current_el, image_base, x0);
	con_color(image_base == ba->physBase ? C_GREEN : C_RED);
	con_printf("LOADED AT PHYSBASE: %s", image_base == ba->physBase ? "YES" : "NO");
	con_color(C_WHITE);
	con_skip();

	con_printf("BOOT_ARGS REV %u VER %u", ba->Revision, ba->Version);
	con_printf(" VIRTBASE %016lX", ba->virtBase);
	con_printf(" PHYSBASE %010lX  MEMSIZE %lX", ba->physBase, ba->memSize);
	con_printf(" TOPOFKERNELDATA %010lX", ba->topOfKernelData);
	con_printf(" VIDEO %08lX %lux%lu ROW %lu DEPTH %lu", ba->Video.v_baseAddr, ba->Video.v_width,
		   ba->Video.v_height, ba->Video.v_rowBytes, ba->Video.v_depth);
	con_printf(" DEVICETREEP %016lX LEN %X", ba->deviceTreeP, ba->deviceTreeLength);
	con_printf(" MEMSIZEACTUAL %lX", ba->memSizeActual);
	con_printf(" CMDLINE:");
	uint32_t clen = 0;
	while (clen < BOOT_LINE_LENGTH && ba->CommandLine[clen])
		clen++;
	con_wrap((uintptr_t)ba->CommandLine, clen, 2);
	con_skip();

	/* ADT copy: "MARBLADT", u32 length, then byte i = i & 0xff for the rest. */
	stage = "adt check";
	uint64_t adt_pa = ba->deviceTreeP - ba->virtBase + ba->physBase;
	volatile uint8_t *adt = (volatile uint8_t *)adt_pa;
	int magic = adt[0] == 'M' && adt[1] == 'A' && adt[2] == 'R' && adt[3] == 'B' &&
		    adt[4] == 'L' && adt[5] == 'A' && adt[6] == 'D' && adt[7] == 'T';
	uint32_t bad = 0;
	for (uint32_t i = 16; i < ba->deviceTreeLength; i++)
		if (adt[i] != (uint8_t)i)
			bad++;
	con_color(magic && !bad ? C_GREEN : C_RED);
	con_printf("ADT AT PA %010lX: MAGIC %s, %u BAD BYTES", adt_pa, magic ? "OK" : "WRONG", bad);
	con_color(C_WHITE);
	con_skip();

	uint64_t lo = ba->physBase, hi = ba->physBase + ba->memSize;
	ram_probe(lo, hi, image_base, ba->topOfKernelData);
	con_skip();

	stage = "tk done";
	finish();
}
