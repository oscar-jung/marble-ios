/*
 * Linked at 0 and run wherever the loader puts it: the code is PC-relative
 * and the build rejects absolute relocations. The flat binary is padded to
 * _end so .bss and the stack exist as zeroed bytes after the loader's copy.
 */
ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { KEEP(*(.text.head)) *(.text*) }
	.rodata : { *(.rodata*) *(.srodata*) }
	.data : { *(.data*) *(.sdata*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(.sbss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x10000;		/* 64 KiB stack */
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_sec_stacks = .;	/* 8 x 4 KiB, one per core started through PSCI */
	. += 8 * 0x1000;
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) }
}
