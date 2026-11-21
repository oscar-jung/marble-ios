/*
 * M5: act as iBoot for XNU (page 5).
 *
 * The payload sits somewhere inside the initrd range ABL reports. With a
 * header-v4 boot image that range starts with the phone's own vendor
 * ramdisk, so the payload is found by scanning for its magic and may be at
 * any 4-byte offset.
 *
 * Steps: validate the header, check that the destination
 * [phys_base, top of boot_args) lies inside one /memory bank and overlaps
 * nothing in use (payload, FDT, framebuffer, this shim, static
 * reserved-memory), copy the segments to phys_base + (vmaddr - virt_base),
 * put the ADT and then boot_args behind the kernel, quiesce the GIC and
 * jump to the entry point with the MMU and caches off, x0 = boot_args.
 */
#include "board.h"
#include "console.h"
#include "fdt.h"
#include "payload.h"
#include "shim.h"

extern char _head[], _end[];

static int magic_at(uintptr_t a)
{
	char m[8];

	for (int i = 0; i < 8; i++)
		m[i] = *(volatile char *)(a + i);
	int marble = 1, sweet = 1;
	for (int i = 0; i < 8; i++) {
		marble &= m[i] == PAYLOAD_MAGIC_MARBLE[i];
		sweet &= m[i] == PAYLOAD_MAGIC_SWEET[i];
	}
	return marble || sweet;
}

/*
 * The payload may start at any byte: ABL puts the vendor ramdisk (arbitrary
 * size) in front of ours. Read aligned 64-bit words and look at each byte.
 */
uintptr_t payload_find(uintptr_t lo, uintptr_t hi)
{
	for (uintptr_t w = lo & ~7UL; w + 8 <= hi; w += 8) {
		uint64_t v = *(volatile uint64_t *)w;
		for (int k = 0; k < 8; k++) {
			uint8_t c = (uint8_t)(v >> (8 * k));
			uintptr_t a = w + k;
			if ((c == 'M' || c == 'S') && a >= lo && a + sizeof(struct payload) <= hi && magic_at(a))
				return a;
		}
	}
	return 0;
}

/*
 * Copy with naturally aligned accesses only (Device memory while the MMU is
 * off): align the destination with byte stores, then move 64-bit words,
 * merging two aligned source words when the source is misaligned. May read
 * up to 7 bytes past the end of the source (same page, always RAM here).
 */
static void copy_mem(uintptr_t dst, uintptr_t src, uint64_t n)
{
	while (n && (dst & 7)) {
		*(volatile uint8_t *)dst++ = *(const volatile uint8_t *)src++;
		n--;
	}
	uint64_t words = n / 8;
	unsigned k = src & 7;
	volatile uint64_t *d = (volatile uint64_t *)dst;
	if (!k) {
		const volatile uint64_t *sw = (const volatile uint64_t *)src;
		for (uint64_t i = 0; i < words; i++)
			d[i] = sw[i];
	} else if (words) {
		const volatile uint64_t *sw = (const volatile uint64_t *)(src & ~7UL);
		uint64_t prev = sw[0];
		for (uint64_t i = 0; i < words; i++) {
			uint64_t next = sw[i + 1];
			d[i] = prev >> (8 * k) | next << (64 - 8 * k);
			prev = next;
		}
	}
	dst += words * 8;
	src += words * 8;
	n -= words * 8;
	while (n--)
		*(volatile uint8_t *)dst++ = *(const volatile uint8_t *)src++;
}

static void zero_mem(uintptr_t dst, uint64_t n)
{
	while (n && (dst & 7)) {
		*(volatile uint8_t *)dst++ = 0;
		n--;
	}
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	while (n--)
		*(volatile uint8_t *)dst++ = 0;
}

static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
	return a0 < b1 && b0 < a1;
}

/* ---- checks against the FDT ---- */

struct span {
	uint64_t lo, hi;
	int hit;		/* reserved-memory: index of the first overlap + 1 */
};

static void resv_check(void *ctxp, int i, const struct fdt_child *c)
{
	struct span *s = ctxp;

	for (uint32_t off = 0; c->reg && off + 16 <= c->reg_len; off += 16) {
		uint64_t base = (uint64_t)be32(c->reg + off) << 32 | be32(c->reg + off + 4);
		uint64_t size = (uint64_t)be32(c->reg + off + 8) << 32 | be32(c->reg + off + 12);
		if (!s->hit && overlaps(s->lo, s->hi, base, base + size))
			s->hit = i + 1;
	}
}

static int inside_memory_bank(uintptr_t fdt, uint64_t lo, uint64_t hi)
{
	uint32_t len = 0;
	uintptr_t reg = fdt_prop(fdt, "memory", "reg", &len);

	for (uint32_t off = 0; reg && off + 16 <= len; off += 16) {
		uint64_t base = (uint64_t)be32(reg + off) << 32 | be32(reg + off + 4);
		uint64_t size = (uint64_t)be32(reg + off + 8) << 32 | be32(reg + off + 12);
		if (lo >= base && hi <= base + size)
			return 1;
	}
	return 0;
}

static void fail(const char *why)
{
	con_color(C_RED);
	con_printf("NOT BOOTING: %s", why);
	con_color(C_WHITE);
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

void loader_page(uintptr_t fdt, const struct fb *fb, uintptr_t p, uintptr_t rd_lo, uintptr_t rd_hi, int wait_s)
{
	stage = "loader header";
	struct payload hdr;
	copy_mem((uintptr_t)&hdr, p, sizeof(hdr));
	con_printf("PAYLOAD %010lX (INITRD +%lX) MAGIC %c%c%c%c%c%c%c%c", p, p - rd_lo, hdr.magic[0],
		   hdr.magic[1], hdr.magic[2], hdr.magic[3], hdr.magic[4], hdr.magic[5], hdr.magic[6], hdr.magic[7]);
	con_printf("VER %u SEGS %u ENTRY %016lX", hdr.version, hdr.nsegs, hdr.entry_va);
	con_printf("VIRT %016lX KEND %016lX", hdr.virt_base, hdr.kernel_end);
	con_printf("PHYS %010lX SIZE %lX (%lu MIB)", hdr.phys_base, hdr.mem_size, hdr.mem_size >> 20);
	con_printf("ADT +%lX SIZE %lX", hdr.adt_off, hdr.adt_size);

	uint64_t avail = rd_hi - p;
	if (hdr.version != 1 || !hdr.nsegs || hdr.nsegs > 64) {
		fail("BAD VERSION OR SEGMENT COUNT");
		return;
	}
	if (sizeof(struct payload) + hdr.nsegs * sizeof(struct payload_seg) > avail ||
	    hdr.adt_off + hdr.adt_size > avail || (hdr.phys_base & (PAGE_16K - 1)) ||
	    hdr.kernel_end < hdr.virt_base) {
		fail("HEADER OUT OF RANGE");
		return;
	}

	uint64_t pa_of_va = hdr.phys_base - hdr.virt_base;	/* pa = va + this */
	uint64_t adt_pa = hdr.kernel_end + pa_of_va;
	uint64_t ba_pa = (adt_pa + hdr.adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uint64_t top = ba_pa + PAGE_16K;
	int entry_ok = 0;

	for (uint32_t i = 0; i < hdr.nsegs; i++) {
		struct payload_seg s;
		copy_mem((uintptr_t)&s, p + sizeof(struct payload) + i * sizeof(s), sizeof(s));
		if (s.file_off + s.file_size > avail || s.vmaddr < hdr.virt_base || s.file_size > s.vmsize ||
		    s.vmaddr + s.vmsize > hdr.kernel_end) {
			fail("SEGMENT OUT OF RANGE");
			return;
		}
		if (hdr.entry_va >= s.vmaddr && hdr.entry_va < s.vmaddr + s.vmsize)
			entry_ok = 1;
		if (i < 12)
			con_printf(" SEG%u VA %016lX PA %010lX FILE %lX MEM %lX", i, s.vmaddr, s.vmaddr + pa_of_va,
				   s.file_size, s.vmsize);
	}
	con_printf("ADT PA %010lX  BOOT_ARGS PA %010lX  TOP %010lX", adt_pa, ba_pa, top);
	if (!entry_ok) {
		fail("ENTRY NOT IN ANY SEGMENT");
		return;
	}

	stage = "loader checks";
	if (!inside_memory_bank(fdt, hdr.phys_base, hdr.phys_base + hdr.mem_size)) {
		fail("PHYS RANGE NOT INSIDE ONE /MEMORY BANK");
		return;
	}
	if (top > hdr.phys_base + hdr.mem_size) {
		fail("KERNEL+ADT+BOOT_ARGS EXCEED MEM_SIZE");
		return;
	}
	uint64_t lo = hdr.phys_base;
	if (overlaps(lo, top, rd_lo, rd_hi) || overlaps(lo, top, fdt, fdt + fdt_totalsize(fdt)) ||
	    overlaps(lo, top, fb->base, fb->base + (uint64_t)fb->stride * fb->height) ||
	    overlaps(lo, top, (uintptr_t)_head, (uintptr_t)_end)) {
		fail("DESTINATION OVERLAPS PAYLOAD/FDT/FB/SHIM");
		return;
	}
	struct span sp = { lo, hdr.phys_base + hdr.mem_size, 0 };
	fdt_foreach_child(fdt, "reserved-memory", resv_check, &sp);
	if (sp.hit) {
		con_printf("OVERLAPS RESERVED-MEMORY NODE #%d", sp.hit - 1);
		fail("XNU RAM OVERLAPS RESERVED-MEMORY");
		return;
	}
	con_color(C_GREEN);
	con_printf("CHECKS OK: ONE BANK, NO OVERLAPS");
	con_color(C_WHITE);

	stage = "loader copy";
	uint64_t t0 = ticks();
	for (uint32_t i = 0; i < hdr.nsegs; i++) {
		struct payload_seg s;
		copy_mem((uintptr_t)&s, p + sizeof(struct payload) + i * sizeof(s), sizeof(s));
		uintptr_t dst = s.vmaddr + pa_of_va;
		copy_mem(dst, p + s.file_off, s.file_size);
		if (s.vmsize > s.file_size)
			zero_mem(dst + s.file_size, s.vmsize - s.file_size);
	}
	copy_mem(adt_pa, p + hdr.adt_off, hdr.adt_size);
	con_printf("COPIED IN %lu MS", (ticks() - t0) / (rsr("cntfrq_el0") / 1000));

	stage = "loader boot_args";
	zero_mem(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = hdr.virt_base;
	ba->physBase = hdr.phys_base;
	ba->memSize = hdr.mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = fb->base;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = fb->stride;
	ba->Video.v_width = fb->width;
	ba->Video.v_height = fb->height;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va;	/* XNU reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)hdr.adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = hdr.cmdline[i];
	ba->CommandLine[BOOT_LINE_LENGTH - 1] = 0;
	ba->memSizeActual = hdr.mem_size;

	uintptr_t entry_pa = hdr.entry_va + pa_of_va;
	con_printf("CMDLINE:");
	uint32_t clen = 0;
	while (clen < BOOT_LINE_LENGTH - 1 && hdr.cmdline[clen])
		clen++;
	con_wrap(ba_pa + __builtin_offsetof(struct boot_args, CommandLine), clen, 1);
	con_color(C_YELLOW);
	con_printf("JUMP TO %010lX (X0=%010lX) IN %d S", entry_pa, ba_pa, wait_s);
	con_color(C_WHITE);
	delay_ms((uint64_t)wait_s * 1000);

	stage = "loader jump";
	gic_quiesce();
	__asm__ volatile("msr daifset, #0xf; dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	fail("ENTRY RETURNED");
}
