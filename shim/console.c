/*
 * Framebuffer text console for bring-up: a 5x7 font scaled up, one
 * 32-bit store per pixel (the MMU is off, so the framebuffer is
 * Device-nGnRnE memory and every access must be naturally aligned).
 */
#include "console.h"

#define GLYPH_W		5
#define GLYPH_H		7
#define CELL_W		((GLYPH_W + 1) * scale)
#define CELL_H		((GLYPH_H + 2) * scale)
#define MARGIN		30
#define KEY_COLS	9

struct glyph {
	char c;
	uint8_t rows[GLYPH_H];	/* bit 4 = leftmost pixel */
};

/* Own 5x7 design. Lower case is drawn as upper case. */
static const struct glyph font[] = {
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'D', { 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
	{ '=', { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 } },
	{ ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
	{ ',', { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
	{ '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } },
	{ '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
	{ '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f } },
	{ '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
	{ ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
	{ '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
	{ '?', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
	{ '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
	{ '@', { 0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0e } },
	{ '"', { 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '[', { 0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e } },
	{ ']', { 0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e } },
	{ '*', { 0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00 } },
	{ '#', { 0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a } },
	{ '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
	{ '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
	{ '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
	{ ';', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x04, 0x08 } },
	{ '|', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ '&', { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d } },
};

static struct fb fb;
static uint32_t fg_color;
static uint32_t cur_y;
static uint32_t scale = 5;
static uint32_t dropped;	/* lines that did not fit since the last clear */

void con_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
	if (x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;
	for (uint32_t j = 0; j < h; j++) {
		volatile uint32_t *row = (volatile uint32_t *)(fb.base + (uintptr_t)(y + j) * fb.stride) + x;
		for (uint32_t i = 0; i < w; i++)
			row[i] = c;
	}
}

void con_init(const struct fb *f, uint32_t bg, uint32_t fg)
{
	(void)bg;
	fb = *f;
	fg_color = fg;
	cur_y = MARGIN;
	dropped = 0;
}

void con_scale(uint32_t s)
{
	scale = s ? s : 1;
}

uint32_t con_cols(void)
{
	return (fb.width - 2 * MARGIN) / CELL_W;
}

uint32_t con_dropped(void)
{
	return dropped;
}

void con_fill(uint32_t c)
{
	con_rect(0, 0, fb.width, fb.height, c);
	cur_y = MARGIN;
	dropped = 0;
}

void con_color(uint32_t fg)
{
	fg_color = fg;
}

void con_bars(void)
{
	uint32_t h = fb.height / 12, y = fb.height - h, w = fb.width / 4;

	con_rect(0, y, w, h, C_RED);
	con_rect(w, y, w, h, C_GREEN);
	con_rect(2 * w, y, w, h, C_BLUE);
	con_rect(3 * w, y, fb.width - 3 * w, h, C_WHITE);
}

static const uint8_t *lookup(char c)
{
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); i++)
		if (font[i].c == c)
			return font[i].rows;
	return 0;
}

static void draw_char(uint32_t x0, uint32_t y0, char c)
{
	const uint8_t *g = lookup(c);

	if (!g || x0 + CELL_W > fb.width || y0 + CELL_H > fb.height)
		return;	/* space, unknown, or off screen */
	for (uint32_t gy = 0; gy < GLYPH_H; gy++)
		for (uint32_t gx = 0; gx < GLYPH_W; gx++)
			if (g[gy] & (0x10 >> gx))
				con_rect(x0 + gx * scale, y0 + gy * scale, scale, scale, fg_color);
}

/* Lines past the bottom (the colour bars stay clear) are dropped and counted, never wrapped over the top. */
static int full(void)
{
	return cur_y + CELL_H > fb.height - fb.height / 12;
}

static uint32_t draw_str(uint32_t col, const char *s)
{
	if (full())
		return col;
	for (; s && *s; s++, col++)
		draw_char(MARGIN + col * CELL_W, cur_y, *s);
	return col;
}

static void newline(void)
{
	if (full())
		dropped++;
	else
		cur_y += CELL_H;
}

void con_line(const char *s)
{
	draw_str(0, s);
	newline();
}

void con_kv(const char *key, const char *val)
{
	draw_str(0, key);
	draw_str(KEY_COLS, val);
	newline();
}

static void hex_str(char *out, uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--, v >>= 4)
		out[i] = "0123456789ABCDEF"[v & 0xf];
	out[digits] = 0;
}

void con_hex(const char *key, uint64_t v)
{
	char buf[17];

	hex_str(buf, v, 16);
	con_kv(key, buf);
}

void con_hex2(const char *key, const char *a, uint64_t va, const char *b, uint64_t vb)
{
	char buf[9];
	uint32_t col;

	draw_str(0, key);
	col = draw_str(KEY_COLS, a);
	hex_str(buf, (uint32_t)va, 8);
	col = draw_str(col, buf) + 1;
	if (b) {
		col = draw_str(col, b);
		hex_str(buf, (uint32_t)vb, 8);
		draw_str(col, buf);
	}
	newline();
}

void con_skip(void)
{
	newline();
}

/* ---- printf subset: %s %c %d %u %x %X with width, 0-pad, '-' and l/ll ---- */

struct out {
	char *buf;
	unsigned n, cap;
};

static void emit(struct out *o, char c)
{
	if (o->n + 1 < o->cap)
		o->buf[o->n++] = c;
}

static void emit_num(struct out *o, uint64_t v, unsigned base, int neg, int width, int zero, int left)
{
	char tmp[24];
	int n = 0;

	do {
		tmp[n++] = "0123456789ABCDEF"[v % base];
		v /= base;
	} while (v);
	if (neg)
		tmp[n++] = '-';
	int pad = width > n ? width - n : 0;
	if (!left)
		while (pad--)
			emit(o, zero ? '0' : ' ');
	while (n)
		emit(o, tmp[--n]);
	if (left)
		while (pad-- > 0)
			emit(o, ' ');
}

static void vformat(struct out *o, const char *f, __builtin_va_list ap)
{
	for (; *f; f++) {
		if (*f != '%') {
			emit(o, *f);
			continue;
		}
		int zero = 0, left = 0, width = 0, lng = 0;
		f++;
		if (*f == '-') {
			left = 1;
			f++;
		}
		if (*f == '0') {
			zero = 1;
			f++;
		}
		while (*f >= '0' && *f <= '9')
			width = width * 10 + (*f++ - '0');
		while (*f == 'l') {
			lng = 1;
			f++;
		}
		switch (*f) {
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);
			int n = 0;
			for (const char *t = s ? s : "(NULL)"; *t; t++, n++)
				emit(o, *t);
			while (left && n++ < width)
				emit(o, ' ');
			break;
		}
		case 'c':
			emit(o, (char)__builtin_va_arg(ap, int));
			break;
		case 'd': {
			int64_t v = lng ? __builtin_va_arg(ap, int64_t) : __builtin_va_arg(ap, int);
			emit_num(o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0, width, zero, left);
			break;
		}
		case 'u':
			emit_num(o, lng ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, unsigned), 10, 0, width, zero, left);
			break;
		case 'x':
		case 'X':
			emit_num(o, lng ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, unsigned), 16, 0, width, zero, left);
			break;
		case '%':
			emit(o, '%');
			break;
		default:
			if (!*f)
				return;
			emit(o, *f);
		}
	}
}

void con_printf(const char *fmt, ...)
{
	char buf[128];
	struct out o = { buf, 0, sizeof(buf) };
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	vformat(&o, fmt, ap);
	__builtin_va_end(ap);
	buf[o.n] = 0;
	draw_str(0, buf);
	newline();
}

void con_wrap(uintptr_t s, uint32_t len, uint32_t indent)
{
	uint32_t cols = con_cols();
	char line[128];
	uint32_t n = 0;

	if (cols > sizeof(line) - 1)
		cols = sizeof(line) - 1;
	if (indent >= cols)
		indent = 0;
	for (uint32_t i = 0; i < len; i++) {
		char c = *(volatile char *)(s + i);
		if (!c)
			c = ' ';	/* string lists: show the NUL as a gap */
		if (n == 0)
			for (; n < indent; n++)
				line[n] = ' ';
		line[n++] = c;
		if (n == cols) {
			line[n] = 0;
			draw_str(0, line);
			newline();
			n = 0;
		}
	}
	if (n) {
		line[n] = 0;
		draw_str(0, line);
		newline();
	}
}
