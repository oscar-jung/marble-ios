/*
 * Board constants. One of BOARD_MARBLE (POCO F5, SM7475) or BOARD_QEMU
 * (qemu-system-aarch64 -M virt, for testing the shim off the phone) is
 * defined by the Makefile.
 *
 * Sources for the marble values: docs/hardware-inventory.md and
 * docs/shim-design.md section 5.
 */
#pragma once

#if defined(BOARD_MARBLE)

#define BOARD_NAME		"POCO F5 MARBLE SM7475"
/* reserved-memory cont_splash_region (cape-sde-display.dtsi) */
#define FB_DEF_BASE		0xb8000000UL
#define FB_REGION_SIZE		0x02b00000UL
#define FB_DEF_W		1080
#define FB_DEF_H		2400
#define FB_DEF_STRIDE		(FB_DEF_W * 4)
/* MDSS MDP (cape-sde-common.dtsi): SSPP pipes, VIG 0-3 then DMA 0-3 */
#define MDP_BASE		0x0ae00000UL
#define MDP_NUM_SSPP		8
#define MDP_SSPP_OFFSETS	{ 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 }
#define PSCI_CONDUIT_SMC	1
#define RESET_DELAY_S		30
#define PAGE_S			25	/* seconds each page stays up (time to photograph it) */
#define LOADER_WAIT_S		15	/* page 5 stays up this long before the jump */
#define PTIMER_TRAPS		1	/* M3, 2026-11-21: Gunyah traps CNTP_* with UNDEF */
/* UFSHCI (cape.dtsi ufshc@1d84000), page 6 read-only probe */
#define UFS_BASE		0x01d84000UL
/* GICv3 (cape.dtsi interrupt-controller@17100000) */
#define GICD_BASE		0x17100000UL
#define GICR_BASE		0x17180000UL
#define GICR_STRIDE		0x40000UL	/* redistributor-stride */
#define GICR_COUNT		8

#elif defined(BOARD_QEMU)

#define BOARD_NAME		"QEMU VIRT"
/*
 * Plain RAM in -M virt with -m 4G (RAM is 0x40000000..0x140000000). Same
 * address as ios-sweet's ADT framebuffer, so its SWEET kernel can be booted
 * through this shim in QEMU (its kernel RAM is 0xb0000000..0xfd000000).
 */
#ifdef QEMU_FB
#define FB_DEF_BASE		QEMU_FB	/* make QEMU_FB_BASE=0xb8000000 for a marble-board kernel */
#else
#define FB_DEF_BASE		0x9c000000UL
#endif
#define FB_REGION_SIZE		0x02b00000UL
#define FB_DEF_W		1080
#define FB_DEF_H		2400
#define FB_DEF_STRIDE		(FB_DEF_W * 4)
#define PSCI_CONDUIT_SMC	0	/* QEMU emulates PSCI behind HVC without EL2/EL3 */
#define RESET_DELAY_S		3
#define PAGE_S			1
#define LOADER_WAIT_S		1
#define PTIMER_TRAPS		0
/* -M virt,gic-version=3 -smp 8 */
#define GICD_BASE		0x08000000UL
#define GICR_BASE		0x080a0000UL
#define GICR_STRIDE		0x20000UL
#define GICR_COUNT		8

#else
#error "define BOARD_MARBLE or BOARD_QEMU"
#endif

/* Arch timer PPIs, the same on both: PPI 11 (virtual) and PPI 14 (EL1 physical, non-secure). */
#define VTIMER_INTID		27
#define PTIMER_INTID		30
#define TEST_SGI		1
#define MAX_CPUS		8
