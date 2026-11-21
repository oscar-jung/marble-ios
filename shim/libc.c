/*
 * The compiler may emit calls to these for struct copies and zeroing even
 * with -ffreestanding. Volatile byte accesses: with the MMU off all memory
 * is Device memory, and bytes are always naturally aligned.
 */
#include <stddef.h>

void *memset(void *d, int c, size_t n)
{
	volatile unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	volatile unsigned char *dp = d;
	const volatile unsigned char *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}
