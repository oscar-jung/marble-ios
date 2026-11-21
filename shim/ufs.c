/*
 * Page 6: can we talk to the internal UFS that ABL booted from? READ-ONLY.
 *
 * Adapted from ios-sweet's shim/ufs.c (MIT, dipa-studio): runs with the MMU
 * off (all memory is Device, so DMA is coherent without cache maintenance),
 * dumps the UFSHCI registers ABL left behind and, if the host is enabled and
 * the link is up, points the transfer request list at our own buffers and
 * issues only TEST UNIT READY, READ CAPACITY(10) and READ(10). There is no
 * SCSI WRITE (or any other data-out command) in this file. The register
 * writes below (UTRLRSR, UTRLBA, doorbell) are controller state, which
 * Android's driver resets on the next boot anyway.
 *
 * marble additions: capacity of every LUN (Qualcomm splits the device into
 * several LUNs; sda = LUN 0 has userdata), UFSHCI version and the 64-bit
 * addressing capability the XNU driver needs for DMA above 4 GiB.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "shim.h"

#ifdef UFS_BASE

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define CAP_64AS	(1u << 24)

#define BLOCK		4096u
#define MAX_LUN		8

/* Plain 32-bit accesses (no writeback forms). */
static inline uint32_t ufs_rd(uint32_t off)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(UFS_BASE + off) : "memory");
	return v;
}

static inline void ufs_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(UFS_BASE + off) : "memory");
}

struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));
static uint8_t ents[32 * BLOCK] __attribute__((aligned(4096)));	/* up to 1024 GPT entries */

/* Sense data of the last CHECK CONDITION (response UPIU: length at 30, fixed-format data at 32). */
static uint8_t sense_key, sense_asc, sense_ascq, sense_len;

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;

	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32p(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t le64p(const volatile uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static uint32_t le32p(const volatile uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One SCSI command (data-in or none) in slot 0. 0 = success, else the failing stage. */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;

	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, device->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host writes it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	dsb();

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t end = ticks() + rsr("cntfrq_el0");	/* 1 s */
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() > end) {
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to its bit) */
			return 1;
		}
	}
	dsb();
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];
	sense_len = ucd.resp[31];
	sense_key = sense_asc = sense_ascq = 0;
	if (ucd.resp[7] == 2 && sense_len >= 14) {
		sense_key = ucd.resp[32 + 2] & 0xf;
		sense_asc = ucd.resp[32 + 12];
		sense_ascq = ucd.resp[32 + 13];
	}
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint8_t lun, uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;

	return scsi(lun, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static const char *fs_type(uint8_t lun, uint32_t lba)
{
	if (read_blocks(lun, lba, 1, data))
		return "READ ERROR";
	const volatile uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "F2FS";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "EXT4";
	if (data[0] == 'A' && data[1] == 'N' && data[2] == 'D')
		return "ANDROID BOOT IMG";
	return "OTHER";
}

static void gpt(uint8_t lun, int show_all)
{
	uint32_t rc = read_blocks(lun, 0, 2, data);

	if (rc) {
		con_printf("LUN%u GPT READ RC=%u", lun, rc);
		return;
	}
	const volatile uint8_t *h = data + BLOCK;
	if (!(h[0] == 'E' && h[1] == 'F' && h[2] == 'I' && h[3] == ' ' && h[4] == 'P' && h[5] == 'A')) {
		con_printf("LUN%u: NO GPT HEADER AT LBA 1", lun);
		return;
	}
	uint32_t elba = (uint32_t)le64p(h + 72), nent = le32p(h + 80), esz = le32p(h + 84);
	if (esz != 128 || nent > sizeof(ents) / 128) {
		con_printf("LUN%u GPT: %u ENTRIES OF %u B (UNSUPPORTED)", lun, nent, esz);
		return;
	}
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(lun, elba, blocks, ents)) {
		con_printf("LUN%u GPT ENTRIES READ ERROR", lun);
		return;
	}
	int used = 0;
	for (uint32_t i = 0; i < nent; i++)
		used += le64p(ents + i * 128 + 32) != 0;
	con_printf("LUN%u GPT: %d PARTITIONS%s", lun, used, show_all ? "" : " (>= 64 MIB SHOWN)");
	for (uint32_t i = 0; i < nent; i++) {
		const volatile uint8_t *e = ents + i * 128;
		uint64_t first = le64p(e + 32), last = le64p(e + 40);
		if (!first)
			continue;
		uint64_t mib = (last - first + 1) >> 8;
		if (!show_all && mib < 64)
			continue;
		char name[24];
		int c = 0;
		for (; c < 23; c++) {
			uint8_t ch = e[56 + 2 * c];
			if (!ch)
				break;
			name[c] = (ch >= 32 && ch < 127) ? (char)ch : '?';
		}
		name[c] = 0;
		int interesting = mib >= 1024;
		con_printf(" %-20s LBA %8lX %7lu MIB%s%s", name, first, mib, interesting ? " " : "",
			   interesting ? fs_type(lun, (uint32_t)first) : "");
	}
}

void page_ufs(void)
{
	stage = "ufs regs";
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	uint32_t ver = ufs_rd(REG_VER);
	con_printf("UFSHCI %08lX VER %X.%X  CAP %08X", UFS_BASE, (ver >> 8) & 0xf, (ver >> 4) & 0xf, cap);
	con_printf(" NUTRS %u  64-BIT DMA %s", (cap & 0x1f) + 1, cap & CAP_64AS ? "YES" : "NO");
	con_printf(" HCE %X HCS %08X IS %08X RSR %X", hce, hcs, ufs_rd(REG_IS), ufs_rd(REG_UTRLRSR));
	con_printf(" ABL UTRLBA %08X%08X DBR %08X", ufs_rd(REG_UTRLBAU), ufs_rd(REG_UTRLBA), ufs_rd(REG_UTRLDBR));

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_color(C_RED);
		con_printf("HOST NOT ENABLED / LINK NOT UP: STOP");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_color(C_RED);
		con_printf("REQUESTS OUTSTANDING: STOP");
		return;
	}

	stage = "ufs take list";
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	/* Never ring a doorbell unless the host uses OUR list (ABL's slot 0 could be anything). */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_color(C_RED);
		con_printf("LIST BASE NOT TAKEN: STOP");
		return;
	}
	con_printf("OUR UTRL AT %08lX, RSR %X", list, ufs_rd(REG_UTRLRSR));

	stage = "ufs tur";
	uint32_t ocs = 0, st = 0, rc = 1;
	static const uint8_t tur[6] = { 0 };
	for (int i = 0; i < 3 && rc; i++) {	/* first command may see a unit attention */
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_printf("TUR LUN0 RC %u OCS %X STATUS %X SENSE LEN %u KEY %X ASC %02X ASCQ %02X", rc, ocs, st,
			   sense_len, sense_key, sense_asc, sense_ascq);
	}
	if (rc) {
		/*
		 * Still failing: wake the device. START STOP UNIT to the UFS Device
		 * well-known LUN (0xD0) with POWER CONDITION = ACTIVE (1). It only
		 * changes the device power mode; no data is written.
		 */
		stage = "ufs ssu";
		static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
		uint32_t r2 = scsi(0xd0, ssu, sizeof(ssu), 0, 0, &ocs, &st);
		con_printf("SSU W-LUN D0 ACTIVE: RC %u OCS %X STATUS %X KEY %X ASC %02X ASCQ %02X", r2, ocs, st,
			   sense_key, sense_asc, sense_ascq);
		for (int i = 0; i < 3 && rc; i++) {
			rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
			con_printf("TUR LUN0 RC %u OCS %X STATUS %X KEY %X ASC %02X ASCQ %02X", rc, ocs, st,
				   sense_key, sense_asc, sense_ascq);
		}
	}

	stage = "ufs capacity";
	static const uint8_t cap10[10] = { 0x25 };
	for (uint8_t lun = 0; lun < MAX_LUN; lun++) {
		rc = scsi(lun, cap10, sizeof(cap10), data, 8, &ocs, &st);
		if (rc)
			con_printf("LUN%u: RC %u OCS %X STATUS %X KEY %X ASC %02X ASCQ %02X", lun, rc, ocs, st,
				   sense_key, sense_asc, sense_ascq);
		else
			con_printf("LUN%u: %8u BLOCKS OF %u = %lu MIB", lun, be32p(data) + 1, be32p(data + 4),
				   ((uint64_t)be32p(data) + 1) * be32p(data + 4) >> 20);
	}

	stage = "ufs gpt";
	con_skip();
	gpt(0, 0);
	con_color(C_GREEN);
	con_printf("READ-ONLY PROBE DONE (NO SCSI WRITE ISSUED)");
	con_color(C_WHITE);
}

#else
void page_ufs(void)
{
	con_printf("NO UFS ON THIS BOARD");
}
#endif
