/*
 * Just enough flattened-device-tree reading for bring-up: top-level nodes
 * only, read in place (the blob is Device memory while the MMU is off, so
 * all loads are naturally aligned: 32-bit tokens, bytes for strings).
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9
#define FDT_MAX_SIZE	(2U << 20)	/* ABL reserves DT_SIZE_2MB */

uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static uint8_t rd8(uintptr_t a)
{
	return *(volatile uint8_t *)a;
}

static uint32_t slen(const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	return n;
}

static int prefix(uintptr_t s, const char *p)
{
	for (; *p; p++, s++)
		if (rd8(s) != (uint8_t)*p)
			return 0;
	return 1;
}

static int streq(uintptr_t s, const char *p)
{
	return prefix(s, p) && rd8(s + slen(p)) == 0;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (rd8(p))
		p++;
	return (p + 1 + 3) & ~3UL;
}

int fdt_valid(uintptr_t fdt)
{
	return fdt && !(fdt & 3) && be32(fdt) == FDT_MAGIC && be32(fdt + 4) <= FDT_MAX_SIZE;
}

uint32_t fdt_totalsize(uintptr_t fdt)
{
	return fdt_valid(fdt) ? be32(fdt + 4) : 0;
}

/*
 * Walk the structure block. For each property of a depth-1 node matching
 * node_prefix call back with (name, value, len); count depth-2 children.
 */
static uintptr_t walk(uintptr_t fdt, const char *node_prefix, const char *want, uint32_t *len, int *children)
{
	if (!fdt_valid(fdt))
		return 0;
	uintptr_t end = fdt + be32(fdt + 4);
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_node = 0, found_node = 0;

	while (p < end) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			if (depth == 2) {
				in_node = prefix(p, node_prefix);
				found_node |= in_node;
			} else if (depth == 3 && in_node && children) {
				(*children)++;
			}
			p = skip_str(p);
			break;
		case FDT_END_NODE:
			if (depth == 2 && in_node && !children)
				return 0;	/* node done, property absent */
			if (depth == 2)
				in_node = 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (depth == 2 && in_node && want && streq(strings + nameoff, want)) {
				*len = l;
				return val;
			}
			p = (val + l + 3) & ~3UL;
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			if (children && !found_node)
				*children = -1;
			return 0;
		}
	}
	if (children && !found_node)
		*children = -1;
	return 0;
}

uintptr_t fdt_prop(uintptr_t fdt, const char *node_prefix, const char *prop, uint32_t *len)
{
	uint32_t l = 0;
	uintptr_t v = walk(fdt, node_prefix, prop, &l, 0);

	if (len)
		*len = v ? l : 0;
	return v;
}

int fdt_prop_u64(uintptr_t fdt, const char *node_prefix, const char *prop, uint64_t *out)
{
	uint32_t len;
	uintptr_t v = fdt_prop(fdt, node_prefix, prop, &len);

	if (!v)
		return 0;
	if (len == 4)
		*out = be32(v);
	else if (len == 8)
		*out = (uint64_t)be32(v) << 32 | be32(v + 4);
	else
		return 0;
	return 1;
}

int fdt_children(uintptr_t fdt, const char *node_prefix)
{
	int n = 0;

	walk(fdt, node_prefix, 0, 0, &n);
	return n;
}

int mem_contains(uintptr_t s, uint32_t len, const char *needle)
{
	uint32_t n = slen(needle);

	for (uint32_t i = 0; i + n <= len; i++)
		if (prefix(s + i, needle))
			return 1;
	return 0;
}

uint32_t fdt_version(uintptr_t fdt)
{
	return fdt_valid(fdt) ? be32(fdt + 20) : 0;
}

uint32_t fdt_boot_cpuid(uintptr_t fdt)
{
	return fdt_valid(fdt) ? be32(fdt + 28) : 0;
}

int fdt_foreach_child(uintptr_t fdt, const char *parent_prefix, fdt_child_fn fn, void *ctx)
{
	if (!fdt_valid(fdt))
		return -1;
	uintptr_t end = fdt + be32(fdt + 4);
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_parent = 0, found = 0, count = 0;
	struct fdt_child c;

	while (p < end) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			if (depth == 2) {
				in_parent = prefix(p, parent_prefix);
				found |= in_parent;
			} else if (depth == 3 && in_parent) {
				c.name = p;
				c.reg = c.size = c.compat = 0;
				c.reg_len = c.size_len = 0;
				c.no_map = 0;
			}
			p = skip_str(p);
			break;
		case FDT_END_NODE:
			if (depth == 3 && in_parent) {
				if (fn)
					fn(ctx, count, &c);
				count++;
			} else if (depth == 2 && in_parent) {
				return count;
			}
			depth--;
			break;
		case FDT_PROP: {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8, name = strings + nameoff;
			if (depth == 3 && in_parent) {
				if (streq(name, "reg")) {
					c.reg = val;
					c.reg_len = l;
				} else if (streq(name, "size")) {
					c.size = val;
					c.size_len = l;
				} else if (streq(name, "no-map")) {
					c.no_map = 1;
				} else if (streq(name, "compatible")) {
					c.compat = val;
				}
			}
			p = (val + l + 3) & ~3UL;
			break;
		}
		case FDT_NOP:
			break;
		default:
			return found ? count : -1;
		}
	}
	return found ? count : -1;
}
