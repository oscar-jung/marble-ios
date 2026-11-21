/*
 * marble-shim entry.
 *
 * The file starts with an arm64 Linux Image header so that Qualcomm ABL (and
 * QEMU's -kernel loader) treat it as a kernel: they enter at offset 0 at EL1
 * (EL2 is Gunyah's on SM7475), MMU and D-cache off, x0 = physical address of
 * the flattened device tree.
 *
 * Everything here is position independent: the image runs wherever the
 * loader put it. The only absolute value is image_size, which is the link
 * address of _end with the image linked at 0, i.e. a plain size.
 */
	.section .text.head, "ax"
	.globl	_head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (no PE header) */

	.text
	.globl	_start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* FDT pointer */
	mrs	x20, CurrentEL
	lsr	x20, x20, #2
	and	x20, x20, #3

	/* Stack inside our own image, SP_ELx for the current EL. */
	msr	spsel, #1
	adrp	x1, _stack_top
	add	x1, x1, :lo12:_stack_top
	mov	sp, x1

	/* Vectors for whichever EL we were entered at. */
	adr	x1, vectors
	cmp	x20, #2
	b.ne	1f
	msr	vbar_el2, x1
	b	2f
1:	msr	vbar_el1, x1
2:	isb

	/* Zero .bss (8-byte stores: memory is Device-nGnRnE with the MMU off). */
	adrp	x1, _bss_start
	add	x1, x1, :lo12:_bss_start
	adrp	x2, _bss_end
	add	x2, x2, :lo12:_bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b

4:	mov	x0, x19
	mov	x1, x20
	adr	x2, _head
	bl	shim_main
5:	wfe
	b	5b

/*
 * PSCI CPU_ON target. x0 = context id = logical cpu index (0..7). Each core
 * gets a 4 KiB stack at _sec_stacks + index * 4 KiB and the same vectors.
 * CPU_ON enters at the caller's EL with the MMU off.
 */
	.globl	secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	and	x19, x0, #7
	adrp	x1, _sec_stacks
	add	x1, x1, :lo12:_sec_stacks
	add	x1, x1, x19, lsl #12
	add	sp, x1, #0x1000
	mrs	x1, CurrentEL
	adr	x2, vectors
	cmp	x1, #(2 << 2)
	b.ne	1f
	msr	vbar_el2, x2
	b	2f
1:	msr	vbar_el1, x2
2:	isb
	mov	x0, x19
	bl	secondary_main
3:	wfe
	b	3b

/*
 * Exception vectors: 16 slots of 0x80 bytes. Each saves x29/x30, puts its
 * slot number in x29 and branches to the common path, which hands
 * exc_handler(slot, frame) a pointer to x0..x30. IRQs return from
 * exc_handler and resume through eret; every other exception is fatal and
 * exc_handler never returns.
 */
.macro	vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl	vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
