/* SPDX-License-Identifier: Apache-2.0 */

#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_display, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_APP_DISPLAY_STATUS) && DT_HAS_CHOSEN(zephyr_display)

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/byteorder.h>

#define DISP_W 135
#define DISP_H 240

/* 8x8 bitmap glyphs for '0'-'9' and '.'; MSB is the leftmost pixel. */
static const uint8_t font_digits[11][8] = {
	{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0 */
	{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
	{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, /* 2 */
	{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
	{0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, /* 4 */
	{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
	{0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
	{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
	{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
	{0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
	{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
};

/* RGB565 colours (drawn big-endian to match the ST7789). */
#define C_RED   0xF800
#define C_BLUE  0x001F
#define C_GREEN 0x07E0
#define C_TEAL  0x0410
#define C_ORANGE 0xFC00
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF

#define SCALE 4                 /* 8x8 glyph -> 32x32 px */
#define GLYPH_PX (8 * SCALE)

static const struct device *disp;
static bool ready;

/* Per-glyph pixel buffer (32x32 RGB565 = 2 KB). */
static uint16_t glyph_buf[GLYPH_PX * GLYPH_PX];
/* One 8px-tall fill strip (135x8 RGB565 ~= 2 KB). */
static uint16_t strip[DISP_W * 8];

static void fill(uint16_t color)
{
	uint16_t be = sys_cpu_to_be16(color);

	for (int i = 0; i < DISP_W * 8; i++) {
		strip[i] = be;
	}

	struct display_buffer_descriptor d = {
		.width = DISP_W, .pitch = DISP_W,
	};
	for (int y = 0; y < DISP_H; y += 8) {
		int h = (y + 8 <= DISP_H) ? 8 : (DISP_H - y);

		d.height = h;
		d.buf_size = (uint32_t)DISP_W * h * 2;
		display_write(disp, 0, y, &d, strip);
	}
}

static void draw_glyph(int x, int y, const uint8_t *g, uint16_t fg, uint16_t bg)
{
	uint16_t fbe = sys_cpu_to_be16(fg), bbe = sys_cpu_to_be16(bg);

	for (int row = 0; row < 8; row++) {
		for (int col = 0; col < 8; col++) {
			uint16_t c = (g[row] & (0x80 >> col)) ? fbe : bbe;

			for (int sr = 0; sr < SCALE; sr++) {
				for (int sc = 0; sc < SCALE; sc++) {
					int px = col * SCALE + sc;
					int py = row * SCALE + sr;

					glyph_buf[py * GLYPH_PX + px] = c;
				}
			}
		}
	}

	struct display_buffer_descriptor d = {
		.buf_size = (uint32_t)GLYPH_PX * GLYPH_PX * 2,
		.width = GLYPH_PX, .height = GLYPH_PX, .pitch = GLYPH_PX,
	};
	display_write(disp, x, y, &d, glyph_buf);
}

/* Draw an octet (0..255) right-aligned in a 3-digit field on one row. */
static void draw_octet(int row, uint8_t val, uint16_t fg, uint16_t bg)
{
	char s[4];
	int n = 0;

	if (val >= 100) {
		s[n++] = '0' + val / 100;
	}
	if (val >= 10) {
		s[n++] = '0' + (val / 10) % 10;
	}
	s[n++] = '0' + val % 10;

	int y = 8 + row * (GLYPH_PX + 8);
	int x = (DISP_W - n * GLYPH_PX) / 2;

	for (int i = 0; i < n; i++) {
		draw_glyph(x + i * GLYPH_PX, y, font_digits[s[i] - '0'], fg, bg);
	}
}

static uint16_t stage_color(enum display_stage stage)
{
	switch (stage) {
	case DISPLAY_STAGE_BOOT:            return C_RED;
	case DISPLAY_STAGE_WIFI_CONNECTING: return C_BLUE;
	case DISPLAY_STAGE_CONNECTED:       return C_GREEN;
	case DISPLAY_STAGE_SERVING:         return C_TEAL;
	default:                            return C_ORANGE;
	}
}

void display_status_init(void)
{
	disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp)) {
		LOG_WRN("Display not ready; status screen disabled");
		disp = NULL;
		return;
	}
	display_blanking_off(disp);
	ready = true;
	fill(C_RED);
	LOG_INF("Status display initialised (%dx%d)", DISP_W, DISP_H);
}

void display_status_stage(enum display_stage stage)
{
	if (!ready) {
		return;
	}
	fill(stage_color(stage));
}

void display_status_ipv4(enum display_stage stage, uint32_t ipv4_be)
{
	if (!ready) {
		return;
	}

	uint16_t bg = stage_color(stage);
	uint16_t fg = C_WHITE;

	fill(bg);

	/* ipv4_be is in network byte order: octet 0 is the MSB on the wire. */
	uint8_t o0 = (ipv4_be) & 0xFF;
	uint8_t o1 = (ipv4_be >> 8) & 0xFF;
	uint8_t o2 = (ipv4_be >> 16) & 0xFF;
	uint8_t o3 = (ipv4_be >> 24) & 0xFF;

	draw_octet(0, o0, fg, bg);
	draw_octet(1, o1, fg, bg);
	draw_octet(2, o2, fg, bg);
	draw_octet(3, o3, fg, bg);
}

void display_status_code(enum display_stage stage, uint32_t code)
{
	if (!ready) {
		return;
	}

	uint16_t bg = stage_color(stage), fg = C_WHITE;

	fill(bg);

	/* Convert to decimal digits (max 10). */
	char digits[10];
	int n = 0;

	if (code == 0) {
		digits[n++] = 0;
	} else {
		char rev[10];
		int r = 0;

		while (code && r < (int)sizeof(rev)) {
			rev[r++] = code % 10;
			code /= 10;
		}
		while (r) {
			digits[n++] = rev[--r];
		}
	}

	int y = (DISP_H - GLYPH_PX) / 2;
	int x = (DISP_W - n * GLYPH_PX) / 2;

	if (x < 0) {
		x = 0;
	}
	for (int i = 0; i < n; i++) {
		draw_glyph(x + i * GLYPH_PX, y, font_digits[(int)digits[i]], fg, bg);
	}
}

#else /* no display configured */

void display_status_init(void) {}
void display_status_stage(enum display_stage stage) { ARG_UNUSED(stage); }
void display_status_ipv4(enum display_stage stage, uint32_t ipv4_be)
{
	ARG_UNUSED(stage);
	ARG_UNUSED(ipv4_be);
}
void display_status_code(enum display_stage stage, uint32_t code)
{
	ARG_UNUSED(stage);
	ARG_UNUSED(code);
}

#endif
