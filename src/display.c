/* SPDX-License-Identifier: Apache-2.0 */

#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_display, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_APP_DISPLAY_STATUS) && DT_HAS_CHOSEN(zephyr_display)

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "font8x8.inc"

#define DISP_W 135
#define DISP_H 240

/* RGB565 colours (drawn big-endian to match the ST7789). */
#define C_RED    0xF800
#define C_BLUE   0x001F
#define C_GREEN  0x07E0
#define C_TEAL   0x0410
#define C_ORANGE 0xFC00
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF

static const struct device *disp;
static bool ready;

/* One 8px-tall fill strip (135x8 RGB565). */
static uint16_t strip[DISP_W * 8];
/* Scratch buffer for one text line at scale 1 (135 wide x 8 tall). */
static uint16_t linebuf[DISP_W * 8];

static void fill(uint16_t color)
{
	uint16_t be = sys_cpu_to_be16(color);

	for (int i = 0; i < DISP_W * 8; i++) {
		strip[i] = be;
	}
	struct display_buffer_descriptor d = { .width = DISP_W, .pitch = DISP_W };

	for (int y = 0; y < DISP_H; y += 8) {
		int h = (y + 8 <= DISP_H) ? 8 : (DISP_H - y);

		d.height = h;
		d.buf_size = (uint32_t)DISP_W * h * 2;
		display_write(disp, 0, y, &d, strip);
	}
}

/* Render one line of text (scale 1, 8px tall) into linebuf and blit at row y. */
static void draw_line(int y, const char *s, uint16_t fg, uint16_t bg)
{
	uint16_t fbe = sys_cpu_to_be16(fg), bbe = sys_cpu_to_be16(bg);

	for (int i = 0; i < DISP_W * 8; i++) {
		linebuf[i] = bbe;
	}
	for (int col = 0; s[col] != '\0'; col++) {
		int x = col * 8;

		if (x + 8 > DISP_W) {
			break; /* clip lines wider than the screen */
		}
		char c = s[col];
		const uint8_t *g = (c >= 0x20 && c < 0x7F) ? font8x8[c - 0x20]
							   : font8x8[0];
		for (int row = 0; row < 8; row++) {
			for (int bit = 0; bit < 8; bit++) {
				if (g[row] & (0x80 >> bit)) {
					linebuf[row * DISP_W + x + bit] = fbe;
				}
			}
		}
	}

	struct display_buffer_descriptor d = {
		.buf_size = (uint32_t)DISP_W * 8 * 2,
		.width = DISP_W, .height = 8, .pitch = DISP_W,
	};
	display_write(disp, 0, y, &d, linebuf);
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
	if (ready) {
		fill(stage_color(stage));
	}
}

void display_status_lines(enum display_stage stage, const char *const *lines,
			  size_t n)
{
	if (!ready) {
		return;
	}

	uint16_t bg = stage_color(stage);

	fill(bg);
	for (size_t i = 0; i < n; i++) {
		if (lines[i] == NULL) {
			continue;
		}
		int y = 4 + (int)i * 11; /* 8px glyph + 3px gap */

		if (y + 8 > DISP_H) {
			break;
		}
		draw_line(y, lines[i], C_WHITE, bg);
	}
}

#else /* no display configured */

void display_status_init(void) {}
void display_status_stage(enum display_stage stage) { ARG_UNUSED(stage); }
void display_status_lines(enum display_stage stage, const char *const *lines,
			  size_t n)
{
	ARG_UNUSED(stage); ARG_UNUSED(lines); ARG_UNUSED(n);
}

#endif
