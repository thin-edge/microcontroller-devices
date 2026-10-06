/* SPDX-License-Identifier: Apache-2.0 */

#include "button_gesture.h"

#include <string.h>

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static void reset_sequence(struct gesture_state *g)
{
	g->presses = 0;
	g->spoiled = false;
	g->erase_armed = false;
}

static bool erase_on(const struct gesture_state *g)
{
	return g->cfg.erase_ms > 0;
}

void gesture_init(struct gesture_state *g, const struct gesture_cfg *cfg)
{
	memset(g, 0, sizeof(*g));
	g->cfg = *cfg;
}

enum gesture gesture_press(struct gesture_state *g, int64_t now_ms)
{
	if (g->pressed) {
		return GESTURE_NONE;
	}
	g->pressed = true;
	g->press_at = now_ms;
	if (g->presses == 0) {
		g->first_press = now_ms;
	}
	return GESTURE_NONE;
}

enum gesture gesture_release(struct gesture_state *g, int64_t now_ms)
{
	if (!g->pressed) {
		return GESTURE_NONE;
	}
	g->pressed = false;

	int64_t held = now_ms - g->press_at;

	if (g->erase_armed ||
	    (erase_on(g) && held >= (int64_t)g->cfg.erase_ms)) {
		reset_sequence(g);
		return GESTURE_ERASE;
	}
	if (held >= (int64_t)g->cfg.short_max_ms) {
		/* A hold that is neither short nor an erase: never a pattern. */
		g->spoiled = true;
	}
	g->presses++;
	g->last_release = now_ms;
	if (g->presses > MAX(g->cfg.press_count, g->cfg.identify_count) ||
	    now_ms - g->first_press > (int64_t)g->cfg.window_ms) {
		g->spoiled = true;
	}
	return GESTURE_NONE;
}

enum gesture gesture_tick(struct gesture_state *g, int64_t now_ms)
{
	if (g->pressed) {
		if (erase_on(g) && !g->erase_armed &&
		    now_ms - g->press_at >= (int64_t)g->cfg.erase_ms) {
			g->erase_armed = true;
			return GESTURE_ERASE_ARMED;
		}
		return GESTURE_NONE;
	}
	if (g->presses > 0 &&
	    now_ms - g->last_release >= (int64_t)g->cfg.quiet_ms) {
		uint32_t n = g->spoiled ? 0 : g->presses;

		reset_sequence(g);
		if (n == 0) {
			return GESTURE_NONE;
		}
		if (n == g->cfg.press_count) {
			return GESTURE_PROVISION;
		}
		if (n == g->cfg.identify_count) {
			return GESTURE_IDENTIFY;
		}
		return GESTURE_NONE;
	}
	return GESTURE_NONE;
}

bool gesture_busy(const struct gesture_state *g)
{
	return g->pressed || g->presses > 0;
}
