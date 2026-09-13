/* SPDX-License-Identifier: Apache-2.0
 *
 * Optional status display for boards with a TFT (e.g. Adafruit Feather
 * ESP32-S2 TFT). Used to relay connectivity state when no serial console is
 * available: a colour-coded background shows the stage, and the device's IPv4
 * address is drawn as large digits so it can be read off the screen.
 *
 * All functions are no-ops when CONFIG_APP_DISPLAY_STATUS is disabled or no
 * display is present, so callers need no guards.
 */
#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stdint.h>

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

/** Set the background colour for a stage (no address shown). */
void display_status_stage(enum display_stage stage);

/**
 * Show a stage plus an IPv4 address, drawn as four large octets stacked
 * vertically (e.g. 192 / 168 / 68 / 74) so it is easy to read back.
 *
 * @param stage background colour.
 * @param ipv4  address in network byte order (as from a struct in_addr).
 */
void display_status_ipv4(enum display_stage stage, uint32_t ipv4_be);

/**
 * Show a decimal number (e.g. a Wi-Fi status/reason code) as large centered
 * digits on a stage-coloured background. Useful for on-screen diagnostics.
 */
void display_status_code(enum display_stage stage, uint32_t code);

#endif /* APP_DISPLAY_H_ */
