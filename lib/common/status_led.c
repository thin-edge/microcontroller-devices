/* SPDX-License-Identifier: Apache-2.0 */

#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#if defined(CONFIG_APP_STATUS_LED) && defined(CONFIG_GPIO) && \
	DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool g_connected;
static bool g_on;

/* 2 Hz blink while disconnected; held steady-on once connected. */
static void led_tick(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (g_connected) {
		g_on = true;
	} else {
		g_on = !g_on;
	}
	(void)gpio_pin_set_dt(&led, g_on);
}

static K_TIMER_DEFINE(led_timer, led_tick, NULL);

void status_led_set_connected(bool connected)
{
	g_connected = connected;
	if (connected) {
		g_on = true;
		(void)gpio_pin_set_dt(&led, 1);
	}
}

static int status_led_init(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}
	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	k_timer_start(&led_timer, K_MSEC(250), K_MSEC(250));
	return 0;
}

SYS_INIT(status_led_init, APPLICATION, 90);

#else /* no status LED on this board / disabled */

void status_led_set_connected(bool connected)
{
	ARG_UNUSED(connected);
}

#endif
