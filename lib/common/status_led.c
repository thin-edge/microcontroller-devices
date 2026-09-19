/* SPDX-License-Identifier: Apache-2.0
 *
 * Status LED: a plain GPIO LED (the board's `led0` alias), or else an
 * addressable RGB LED (the `led-strip` alias, e.g. a WS2812). Both show the
 * same blink patterns; the RGB LED also shows each mode in its own colour.
 */

#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#if defined(CONFIG_GPIO) && DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#define HAS_GPIO_LED 1
#else
#define HAS_GPIO_LED 0
#endif
#if defined(CONFIG_LED_STRIP) && DT_NODE_HAS_STATUS(DT_ALIAS(led_strip), okay)
#define HAS_RGB_LED 1
#else
#define HAS_RGB_LED 0
#endif

#if defined(CONFIG_APP_STATUS_LED) && (HAS_GPIO_LED || HAS_RGB_LED)

#include <zephyr/init.h>

#if HAS_GPIO_LED
#include <zephyr/drivers/gpio.h>
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#else
#include <zephyr/drivers/led_strip.h>
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
#endif

/* Each pattern is a cycle of alternating on/off segments in milliseconds,
 * starting with "on". A single segment means a constant level. The timings and
 * colours are fixed (not Kconfig) so every device in the field looks the same.
 */
struct pattern {
	const uint16_t *seg;
	uint8_t n;
	bool level; /* level for a constant (n == 1) pattern */
	uint8_t r, g, b; /* colour when "on" (RGB LED only) */
};

static const uint16_t seg_const[] = { 1000 };
static const uint16_t seg_disconnected[] = { 250, 250 };
static const uint16_t seg_provisioning[] = { 100, 150, 100, 1500 };
static const uint16_t seg_identify[] = { 100, 100 };
static const uint16_t seg_erase[] = { 50, 50 };

/* About 1/8 brightness: a WS2812 at full power is glaring on a desk. */
static const struct pattern patterns[] = {
	[STATUS_LED_CONNECTED] = { seg_const, 1, true, 0, 32, 0 },       /* green */
	[STATUS_LED_DISCONNECTED] = { seg_disconnected, 2, false, 32, 16, 0 }, /* amber */
	[STATUS_LED_PROVISIONING] = { seg_provisioning, 4, false, 0, 0, 40 }, /* blue */
	[STATUS_LED_IDENTIFY] = { seg_identify, 2, false, 32, 32, 32 },  /* white */
	[STATUS_LED_ERASE_ARMED] = { seg_erase, 2, false, 40, 0, 0 },    /* red */
	[STATUS_LED_OFF] = { seg_const, 1, false, 0, 0, 0 },
};

/* The pattern runs on the system work queue rather than a timer: an RGB
 * LED update goes through a bus driver (I2S/SPI) that must not be called from
 * interrupt context. */
static struct k_spinlock lock;
static enum status_led_mode base_mode = STATUS_LED_DISCONNECTED;
static enum status_led_mode shown_mode = STATUS_LED_DISCONNECTED;
static int64_t flash_until; /* 0 = no temporary pattern */
static uint8_t seg_idx;
static bool restart; /* show the current pattern from its first segment */
static bool ready;

static void led_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_fn);

static void led_write(enum status_led_mode mode, bool on)
{
#if HAS_GPIO_LED
	ARG_UNUSED(mode);
	(void)gpio_pin_set_dt(&led, on);
#else
	const struct pattern *p = &patterns[mode];
	struct led_rgb px = { 0 };

	if (on) {
		px.r = p->r;
		px.g = p->g;
		px.b = p->b;
	}
	(void)led_strip_update_rgb(strip, &px, 1);
#endif
}

static void led_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (flash_until && k_uptime_get() >= flash_until) {
		flash_until = 0;
		shown_mode = base_mode;
		restart = true;
	}
	if (restart) {
		restart = false;
		seg_idx = 0;
	} else if (patterns[shown_mode].n > 1) {
		seg_idx = (seg_idx + 1) % patterns[shown_mode].n;
	}

	const struct pattern *p = &patterns[shown_mode];
	enum status_led_mode mode = shown_mode;
	bool on = (p->n == 1) ? p->level : (seg_idx % 2) == 0;
	int32_t next_ms = -1;

	if (p->n > 1) {
		next_ms = p->seg[seg_idx];
	} else if (flash_until) {
		/* Constant: re-check only when a temporary pattern must end. */
		next_ms = 50;
	}
	k_spin_unlock(&lock, key);

	led_write(mode, on);
	if (next_ms >= 0) {
		(void)k_work_reschedule(&led_work, K_MSEC(next_ms));
	}
}

/* Show @p mode from its first segment, now. */
static void show_locked(enum status_led_mode mode)
{
	shown_mode = mode;
	restart = true;
	(void)k_work_reschedule(&led_work, K_NO_WAIT);
}

void status_led_set_mode(enum status_led_mode mode)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	base_mode = mode;
	if (ready && !flash_until && mode != shown_mode) {
		show_locked(mode);
	}
	k_spin_unlock(&lock, key);
}

void status_led_flash(enum status_led_mode mode, uint32_t ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (ready) {
		flash_until = k_uptime_get() + ms;
		show_locked(mode);
	}
	k_spin_unlock(&lock, key);
}

void status_led_set_connected(bool connected)
{
	status_led_set_mode(connected ? STATUS_LED_CONNECTED
				      : STATUS_LED_DISCONNECTED);
}

bool status_led_present(void)
{
	return ready;
}

static int status_led_init(void)
{
#if HAS_GPIO_LED
	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}
	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
#else
	if (!device_is_ready(strip)) {
		return 0;
	}
#endif
	k_spinlock_key_t key = k_spin_lock(&lock);

	ready = true;
	show_locked(base_mode);
	k_spin_unlock(&lock, key);
	return 0;
}

SYS_INIT(status_led_init, APPLICATION, 90);

#else /* no status LED on this board / disabled */

void status_led_set_connected(bool connected)
{
	ARG_UNUSED(connected);
}

void status_led_set_mode(enum status_led_mode mode)
{
	ARG_UNUSED(mode);
}

void status_led_flash(enum status_led_mode mode, uint32_t ms)
{
	ARG_UNUSED(mode);
	ARG_UNUSED(ms);
}

bool status_led_present(void)
{
	return false;
}

#endif
