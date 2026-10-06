/* SPDX-License-Identifier: Apache-2.0 */

#include "button.h"
#include "button_gesture.h"
#include "net.h"
#if defined(CONFIG_APP_PROV_HANDOFF)
#include "prov_handoff.h"
#endif
#if defined(CONFIG_APP_IDENTIFY)
#include "identify.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(app_button, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)

#define BTN_DEBOUNCE_MS 30
#define BTN_TICK_MS     50

#if defined(CONFIG_APP_PROV_HANDOFF)
#define PROV_PRESS_COUNT CONFIG_APP_WIFI_PROV_PRESS_COUNT
#define PRESS_WINDOW_MS  CONFIG_APP_WIFI_PROV_PRESS_WINDOW_MS
#define ERASE_HOLD_MS    (CONFIG_APP_WIFI_PROV_ERASE_HOLD_S * 1000)
#else
#define PROV_PRESS_COUNT 0 /* off */
#define PRESS_WINDOW_MS  2000
#define ERASE_HOLD_MS    0 /* off */
#endif

#if defined(CONFIG_APP_IDENTIFY)
#define IDENTIFY_PRESS_COUNT CONFIG_APP_IDENTIFY_PRESS_COUNT
#else
#define IDENTIFY_PRESS_COUNT 0 /* off */
#endif

BUILD_ASSERT(PROV_PRESS_COUNT == 0 || PROV_PRESS_COUNT != IDENTIFY_PRESS_COUNT,
	     "APP_IDENTIFY_PRESS_COUNT must differ from "
	     "APP_WIFI_PROV_PRESS_COUNT");

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback btn_cb;
static struct k_work_delayable btn_work;
static struct gesture_state gst;
static bool btn_level;
static bool btn_started;

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Debounce: act once the line has been quiet for BTN_DEBOUNCE_MS. */
	(void)k_work_reschedule_for_queue(app_net_workq(), &btn_work,
					  K_MSEC(BTN_DEBOUNCE_MS));
}

static void handle_gesture(enum gesture g)
{
	switch (g) {
	case GESTURE_NONE:
		break;
	case GESTURE_IDENTIFY:
#if defined(CONFIG_APP_IDENTIFY)
		app_identify_gesture();
#endif
		break;
	default:
#if defined(CONFIG_APP_PROV_HANDOFF)
		app_prov_handle_gesture(g);
#endif
		break;
	}
}

static void btn_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int64_t now = k_uptime_get();
	bool level = gpio_pin_get_dt(&btn) > 0;
	enum gesture g = GESTURE_NONE;

	/* Per-press log lines: on boards without an LED the console is the only
	 * feedback an operator has on what the device saw. */
	static int64_t pressed_at;
	static bool seq_active;
	static bool seq_matched;

	if (level != btn_level) {
		btn_level = level;
		if (level) {
			pressed_at = now;
			seq_active = true;
			LOG_INF("sw0 pressed");
		} else {
			LOG_INF("sw0 released after %u ms",
				(unsigned int)(now - pressed_at));
		}
		g = level ? gesture_press(&gst, now) : gesture_release(&gst, now);
	}
	if (g == GESTURE_NONE) {
		g = gesture_tick(&gst, now);
	}
	if (g != GESTURE_NONE) {
		seq_matched = true;
	}
	handle_gesture(g);
	if (gesture_busy(&gst)) {
		(void)k_work_reschedule_for_queue(app_net_workq(), &btn_work,
						  K_MSEC(BTN_TICK_MS));
	} else if (seq_active) {
		if (!seq_matched) {
			LOG_INF("sw0: not a gesture, ignored");
		}
		seq_active = false;
		seq_matched = false;
	}
}

void app_button_start(void)
{
	static const struct gesture_cfg cfg = {
		.press_count = PROV_PRESS_COUNT,
		.identify_count = IDENTIFY_PRESS_COUNT,
		.window_ms = PRESS_WINDOW_MS,
		.short_max_ms = 1000,
		.quiet_ms = 700,
		.erase_ms = ERASE_HOLD_MS,
	};

	if (btn_started || !gpio_is_ready_dt(&btn)) {
		return;
	}
	gesture_init(&gst, &cfg);
	k_work_init_delayable(&btn_work, btn_work_fn);
	if (gpio_pin_configure_dt(&btn, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH) != 0) {
		LOG_ERR("sw0 configuration failed");
		return;
	}
	btn_level = gpio_pin_get_dt(&btn) > 0;
	gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
	(void)gpio_add_callback(btn.port, &btn_cb);
	btn_started = true;
}

#else /* no sw0 on this board */

void app_button_start(void)
{
}

#endif
