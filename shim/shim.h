/* Shared helpers between main.c, smp.c and gic.c. */
#pragma once
#include <stdint.h>

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb sy" ::: "memory")
#define FIELD(v, lo)	(((v) >> (lo)) & 0xf)

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern const char *volatile stage;	/* shown on the panic screen */

uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t ticks(void);
void delay_ms(uint64_t ms);
const char *cpu_part_name(uint64_t midr);

/* smp.c */
void page_smp(uintptr_t fdt);
void secondary_main(uint64_t idx);

/* gic.c */
void page_gic(void);
void gic_ptimer_test(void);
void gic_irq(void);
/* Interrupts off at the GIC and the core, timers stopped: state handed to the next stage. */
void gic_quiesce(void);

/* usb.c: log channel to the host (tools/usblog.py) */
int usb_start(void);
void usb_wait_host(uint32_t ms);
void usb_poll(void);
void usb_stop(void);
int usb_configured(void);

/* ufs.c: page 6, read-only UFS probe */
void page_ufs(void);

/* loader.c */
struct fb;
uintptr_t payload_find(uintptr_t lo, uintptr_t hi);
/* Returns only if the payload is rejected (the reason is on the screen). */
void loader_page(uintptr_t fdt, const struct fb *fb, uintptr_t payload, uintptr_t rd_lo, uintptr_t rd_hi, int wait_s);
