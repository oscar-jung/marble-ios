/*
 * M2: start every core listed under /cpus with PSCI CPU_ON, let it report
 * its ID registers, and switch it off again with CPU_OFF.
 *
 * Caches and MMU are off on every core, so plain (Device) stores to the
 * shared table are visible to the boot core without maintenance.
 */
#include "board.h"
#include "console.h"
#include "fdt.h"
#include "shim.h"

extern char secondary_entry[];

struct core_report {
	volatile uint64_t arrived;	/* 1 once the core has written the rest */
	volatile uint64_t midr, mpidr, isar1, isar2, mmfr0, mmfr2, pfr0, el;
};

static struct core_report report[MAX_CPUS];

struct cpus_ctx {
	uint64_t mpidr[MAX_CPUS];
	int n;
};

static int name_is_cpu(uintptr_t name)
{
	return *(volatile char *)name == 'c' && *(volatile char *)(name + 1) == 'p' &&
	       *(volatile char *)(name + 2) == 'u' && *(volatile char *)(name + 3) == '@';
}

static void collect_cpu(void *ctxp, int i, const struct fdt_child *c)
{
	struct cpus_ctx *ctx = ctxp;

	(void)i;
	if (!name_is_cpu(c->name) || !c->reg || ctx->n == MAX_CPUS)
		return;
	ctx->mpidr[ctx->n++] = c->reg_len >= 8 ? (uint64_t)be32(c->reg) << 32 | be32(c->reg + 4) : be32(c->reg);
}

static void fill(struct core_report *r)
{
	r->midr = rsr("midr_el1");
	r->mpidr = rsr("mpidr_el1");
	r->isar1 = rsr("id_aa64isar1_el1");
	r->isar2 = rsr("s3_0_c0_c6_2");		/* ID_AA64ISAR2_EL1 */
	r->mmfr0 = rsr("id_aa64mmfr0_el1");
	r->mmfr2 = rsr("id_aa64mmfr2_el1");
	r->pfr0 = rsr("id_aa64pfr0_el1");
	r->el = (rsr("CurrentEL") >> 2) & 3;
	dsb();
	r->arrived = 1;
	dsb();
}

void secondary_main(uint64_t idx)
{
	if (idx < MAX_CPUS)
		fill(&report[idx]);
	psci(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static const char *psci_err(int64_t r)
{
	switch (r) {
	case 0: return "OK";
	case -1: return "NOTSUP";
	case -2: return "INVAL";
	case -3: return "DENIED";
	case -4: return "ALRDYON";
	case -5: return "PENDING";
	case -6: return "INTERNAL";
	case -9: return "BADADDR";
	default: return "ERR";
	}
}

static void row(int i, uint64_t target, const char *what, const struct core_report *r)
{
	if (!r->arrived) {
		con_color(C_RED);
		con_printf("%d %06lX %-8s NO REPORT", i, target, what);
		con_color(C_WHITE);
		return;
	}
	/* PAC: APA or API or APA3; 16K: TGran16 != 0; VA: MMFR2.VARange; AdvSIMD/SVE from PFR0 */
	int pac = FIELD(r->isar1, 4) || FIELD(r->isar1, 8) || FIELD(r->isar2, 12);
	int tg16 = FIELD(r->mmfr0, 20) != 0;
	con_color(pac && tg16 ? C_GREEN : C_RED);
	con_printf("%d %06lX %-8s %-4s R%luP%lu APA%lu GPA%lu TG16=%lu PA%lu SVE%lu EL%lu", i, target, what,
		   cpu_part_name(r->midr), (r->midr >> 20) & 0xf, r->midr & 0xf, FIELD(r->isar1, 4),
		   FIELD(r->isar1, 24), FIELD(r->mmfr0, 20), FIELD(r->mmfr0, 0), FIELD(r->pfr0, 32), r->el);
	con_color(C_WHITE);
}

void page_smp(uintptr_t fdt)
{
	struct cpus_ctx cpus;
	uint64_t self = rsr("mpidr_el1") & 0xff00ffffffUL;	/* affinity fields only */
	int64_t ret[MAX_CPUS];

	cpus.n = 0;
	stage = "smp fdt";
	fdt_foreach_child(fdt, "cpus", collect_cpu, &cpus);
	con_printf("/CPUS: %d CPU@ NODES. BOOT MPIDR %08lX", cpus.n, rsr("mpidr_el1"));
	con_printf("#  TARGET CPU_ON   CORE REV  PAC   16K  PA  SVE EL");
	if (!cpus.n)
		return;

	for (int i = 0; i < cpus.n; i++) {
		report[i].arrived = 0;
		ret[i] = 1;	/* "boot core", not called */
	}
	dsb();
	for (int i = 0; i < cpus.n; i++) {
		if (cpus.mpidr[i] == self) {
			fill(&report[i]);
			continue;
		}
		stage = "smp cpu_on";
		ret[i] = (int64_t)psci(PSCI_CPU_ON, cpus.mpidr[i], (uintptr_t)secondary_entry, (uint64_t)i);
		/* Wait for the report (cores come up one at a time: easier to tell who hung). */
		uint64_t end = ticks() + rsr("cntfrq_el0") / 2;
		while (ret[i] == 0 && !report[i].arrived && ticks() < end)
			;
	}
	stage = "smp report";
	for (int i = 0; i < cpus.n; i++)
		row(i, cpus.mpidr[i], ret[i] == 1 ? "BOOT" : psci_err(ret[i]), &report[i]);

	/* After CPU_OFF every secondary should read back OFF (1). */
	delay_ms(50);
	stage = "smp affinity";
	char line[64];
	int p = 0;
	for (const char *t = "AFFINITY_INFO:"; *t; t++)
		line[p++] = *t;
	for (int i = 0; i < cpus.n && p < 60; i++) {
		int64_t a = (int64_t)psci(PSCI_AFFINITY_INFO, cpus.mpidr[i], 0, 0);
		line[p++] = ' ';
		line[p++] = a == 0 ? 'N' : a == 1 ? 'F' : a == 2 ? 'P' : '?';	/* oN, oFf, Pending */
	}
	line[p] = 0;
	con_printf("%s  (N=ON F=OFF P=PENDING)", line);
	con_printf("MIDR/MPIDR PER CORE:");
	for (int i = 0; i < cpus.n; i++)
		if (report[i].arrived)
			con_printf(" %d MIDR %08lX MPIDR %08lX MMFR2 %lX", i, report[i].midr, report[i].mpidr,
				   report[i].mmfr2);
}
