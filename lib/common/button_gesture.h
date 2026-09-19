/* SPDX-License-Identifier: Apache-2.0
 *
 * sw0 gesture classifier for BLE provisioning: recognizes the provisioning
 * pattern (exactly N short presses within a window, then a quiet gap) and the
 * erase hold (one long continuous press). Pure logic with no Zephyr
 * dependency, fed with debounced press/release edges and periodic ticks, so it
 * can be tested on the host (see tests/button_gesture/).
 */
#ifndef APP_BUTTON_GESTURE_H_
#define APP_BUTTON_GESTURE_H_

#include <stdbool.h>
#include <stdint.h>

enum gesture {
	GESTURE_NONE,
	GESTURE_PROVISION,   /**< the N-press pattern completed */
	GESTURE_ERASE_ARMED, /**< a hold has passed the erase threshold */
	GESTURE_ERASE,       /**< an armed erase hold was released */
};

struct gesture_cfg {
	uint32_t press_count;  /**< presses in the pattern */
	uint32_t window_ms;    /**< first press to last release */
	uint32_t short_max_ms; /**< a press at least this long is not "short" */
	uint32_t quiet_ms;     /**< no press for this long ends a sequence */
	uint32_t erase_ms;     /**< hold this long to arm the erase */
};

struct gesture_state {
	struct gesture_cfg cfg;
	bool pressed;
	bool erase_armed;
	bool spoiled; /* the current sequence can no longer be the pattern */
	uint32_t presses;
	int64_t first_press;
	int64_t press_at;
	int64_t last_release;
};

void gesture_init(struct gesture_state *g, const struct gesture_cfg *cfg);

/** Feed a debounced edge at time @p now_ms. */
enum gesture gesture_press(struct gesture_state *g, int64_t now_ms);
enum gesture gesture_release(struct gesture_state *g, int64_t now_ms);

/** Advance time; call periodically while gesture_busy(). */
enum gesture gesture_tick(struct gesture_state *g, int64_t now_ms);

/** @return true while a press or an unfinished sequence needs ticks. */
bool gesture_busy(const struct gesture_state *g);

#endif /* APP_BUTTON_GESTURE_H_ */
