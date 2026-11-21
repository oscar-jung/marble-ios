#pragma once
#include <stdint.h>

/* 0xAARRGGBB, i.e. a8r8g8b8 as the splash surface is expected to be. */
#define C_BLACK		0xff000000U
#define C_WHITE		0xffffffffU
#define C_RED		0xffff0000U
#define C_GREEN		0xff00ff00U
#define C_BLUE		0xff0000ffU
#define C_YELLOW	0xffffff00U
#define C_GREY		0xff808080U
#define C_NAVY		0xff000040U
#define C_DARKRED	0xff600000U

struct fb {
	uintptr_t base;
	uint32_t width, height, stride;	/* stride in bytes */
};

void con_init(const struct fb *fb, uint32_t bg, uint32_t fg);
void con_fill(uint32_t color);
void con_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/* Colour bars across the bottom: R, G, B, white. Shows a swapped pixel format at a glance. */
void con_bars(void);
void con_color(uint32_t fg);
void con_line(const char *s);
void con_kv(const char *key, const char *val);
void con_hex(const char *key, uint64_t v);
void con_hex2(const char *key, const char *a, uint64_t va, const char *b, uint64_t vb);
void con_skip(void);
/* Glyph scale (5 = default, 3 = dense pages). */
void con_scale(uint32_t s);
uint32_t con_cols(void);
uint32_t con_dropped(void);
/* One line; %s %c %d %u %x with width, 0-pad, '-' and l. */
void con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Text of len bytes at s (may be Device memory), wrapped to the screen width. */
void con_wrap(uintptr_t s, uint32_t len, uint32_t indent);
/* Every character the console prints, as text (usb.c sends it to the host). */
void con_tee(char c);
