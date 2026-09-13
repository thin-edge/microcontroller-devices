/* SPDX-License-Identifier: Apache-2.0
 *
 * Optional status/diagnostics display for boards with a TFT (e.g. Adafruit
 * Feather ESP32-S2 TFT). Used to relay connectivity state and network details
 * when no serial console is available: a colour-coded background shows the
 * stage, and text lines show IP/gateway/netmask/Wi-Fi RSSI/state/etc.
 *
 * All functions are no-ops when CONFIG_APP_DISPLAY_STATUS is disabled or no
 * display is present, so callers need no guards.
 */
#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stddef.h>

/** Initialise the display. Safe to call when no display is configured. */
void display_status_init(void);

/** Stage indicator (background colour). */
enum display_stage {
	DISPLAY_STAGE_BOOT,		/* red    */
	DISPLAY_STAGE_WIFI_CONNECTING,	/* blue   */
	DISPLAY_STAGE_CONNECTED,	/* green  */
	DISPLAY_STAGE_SERVING,		/* teal   */
	DISPLAY_STAGE_ERROR,		/* orange */
};

/** Set the background colour for a stage (no text). */
void display_status_stage(enum display_stage stage);

/**
 * Fill the background for a stage and render text diagnostic lines (small
 * 8px font, one per row). Lines wider than the screen are clipped.
 *
 * @param stage background colour.
 * @param lines array of NUL-terminated strings (NULL entries skipped).
 * @param n     number of entries in @p lines.
 */
void display_status_lines(enum display_stage stage, const char *const *lines,
			  size_t n);

#endif /* APP_DISPLAY_H_ */
