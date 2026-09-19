/* SPDX-License-Identifier: Apache-2.0 */

#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#if defined(CONFIG_APP_STATUS_LED) && defined(CONFIG_GPIO) && \
	DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Each pattern is a cycle of alternating on/off segments in milliseconds,
 * starting with "on". A single segment means a constant level. The timings are
 * fixed (not Kconfig) so every device in the field looks the same. */
struct pattern {
	const uint16_t *seg;
	uint8_t n;
	bool level; /* level for a constant (n == 1) pattern */
};

static const uint16_t seg_const[] = { 1000 };
static const uint16_t seg_disconnected[] = { 250, 250 };
static const uint16_t seg_provisioning[] = { 100, 150, 100, 1500 };
static const uint16_t seg_identify[] = { 100, 100 };
static const uint16_t seg_erase[] = { 50, 50 };

static const struct pattern patterns[] = {
	[STATUS_LED_CONNECTED] = { seg_const, 1, true },
	[STATUS_LED_DISCONNECTED] = { seg_disconnected, 2, false },
	[STATUS_LED_PROVISIONING] = { seg_provisioning, 4, false },
	[STATUS_LED_IDENTIFY] = { seg_identify, 2, false },
	[STATUS_LED_ERASE_ARMED] = { seg_erase, 2, false },
	[STATUS_LED_OFF] = { seg_const, 1, false },
};

static struct k_spinlock lock;
static enum status_led_mode base_mode = STATUS_LED_DISCONNECTED;
static enum status_led_mode shown_mode = STATUS_LED_DISCONNECTED;
static int64_t flash_until; /* 0 = no temporary pattern */
static uint8_t seg_idx;
static bool ready;

static void led_timer_fn(struct k_timer *t);
static K_TIMER_DEFINE(led_timer, led_timer_fn, NULL);

/* Drive the LED for the current segment and arm the timer for its end. */
static void show_segment(void)
{
	const struct pattern *p = &patterns[shown_mode];

	if (p->n == 1) {
		(void)gpio_pin_set_dt(&led, p->level);
		/* Constant: re-check only when a temporary pattern must end. */
		if (flash_until) {
			k_timer_start(&led_timer, K_MSEC(50), K_NO_WAIT);
		}
		return;
	}
	(void)gpio_pin_set_dt(&led, (seg_idx % 2) == 0);
	k_timer_start(&led_timer, K_MSEC(p->seg[seg_idx]), K_NO_WAIT);
}

static void apply_locked(enum status_led_mode mode)
{
	if (mode != shown_mode) {
		shown_mode = mode;
		seg_idx = 0;
		show_segment();
	}
}

static void led_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (flash_until && k_uptime_get() >= flash_until) {
		flash_until = 0;
		shown_mode = base_mode;
		seg_idx = 0;
	} else if (patterns[shown_mode].n > 1) {
		seg_idx = (seg_idx + 1) % patterns[shown_mode].n;
	}
	show_segment();
	k_spin_unlock(&lock, key);
}

void status_led_set_mode(enum status_led_mode mode)
{
	if (!ready) {
		base_mode = mode;
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	base_mode = mode;
	if (!flash_until) {
		apply_locked(mode);
	}
	k_spin_unlock(&lock, key);
}

void status_led_flash(enum status_led_mode mode, uint32_t ms)
{
	if (!ready) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	flash_until = k_uptime_get() + ms;
	shown_mode = STATUS_LED_OFF; /* force a restart of the pattern */
	apply_locked(mode);
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
	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}
	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	ready = true;
	k_spinlock_key_t key = k_spin_lock(&lock);

	shown_mode = base_mode;
	seg_idx = 0;
	show_segment();
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
