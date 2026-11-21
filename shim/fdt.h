#pragma once
#include <stdint.h>

int fdt_valid(uintptr_t fdt);
uint32_t fdt_totalsize(uintptr_t fdt);
/*
 * Property of a top-level node whose name (unit address included) starts
 * with node_prefix, e.g. "chosen" or "memory". Returns the value's address
 * or 0, and its length in *len.
 */
uintptr_t fdt_prop(uintptr_t fdt, const char *node_prefix, const char *prop, uint32_t *len);
/* A 32- or 64-bit cell value from fdt_prop (big-endian). 0 if absent or another size. */
int fdt_prop_u64(uintptr_t fdt, const char *node_prefix, const char *prop, uint64_t *out);
/* Number of direct children of a top-level node, -1 if the node is absent. */
int fdt_children(uintptr_t fdt, const char *node_prefix);
/* Does the NUL-terminated string list in [s, s+len) contain needle as a substring? */
int mem_contains(uintptr_t s, uint32_t len, const char *needle);
uint32_t be32(uintptr_t a);

/* A direct child of a top-level node, as fdt_foreach_child reports it. */
struct fdt_child {
	uintptr_t name;			/* NUL-terminated, in the blob */
	uintptr_t reg;			/* 0 if absent */
	uint32_t reg_len;
	uintptr_t size;			/* reserved-memory "size" (dynamic regions) */
	uint32_t size_len;
	uintptr_t compat;		/* first string of "compatible" */
	int no_map;
};

typedef void (*fdt_child_fn)(void *ctx, int index, const struct fdt_child *c);

/* Calls fn for each direct child of the top-level node matching parent_prefix; returns the count or -1. */
int fdt_foreach_child(uintptr_t fdt, const char *parent_prefix, fdt_child_fn fn, void *ctx);
/* Header fields */
uint32_t fdt_version(uintptr_t fdt);
uint32_t fdt_boot_cpuid(uintptr_t fdt);
