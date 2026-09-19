/* SPDX-License-Identifier: Apache-2.0 */

#include "prov_handoff.h"
#include "boot_request.h"
#include "button_gesture.h"
#include "net.h"
#include "prov_identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/wifi_credentials.h>
#include <string.h>

LOG_MODULE_REGISTER(app_prov, CONFIG_LOG_DEFAULT_LEVEL);

#define ACK_MS          1000 /* LED acknowledgement before a reboot */
#define BTN_DEBOUNCE_MS 30
#define BTN_TICK_MS     50

static FUNC_NORETURN void reboot_now(void)
{
	log_flush(); /* deferred logging: print before the reset */
	boot_request_reboot();
	CODE_UNREACHABLE;
}

/* Set the boot request; false when there is no provisioner to hand over to. */
static bool request_provisioner(enum boot_request_reason reason)
{
	if (!boot_request_provisioner_present()) {
		LOG_ERR("No Wi-Fi provisioner is flashed (prov partition is "
			"empty); flash it with scripts/flash.sh");
		return false;
	}
	int rc = boot_request_set(BOOT_REQUEST_PROVISIONER, reason);

	if (rc != 0) {
		LOG_ERR("Could not set the boot request (%d)", rc);
		return false;
	}
	return true;
}

void app_prov_handoff_no_credentials(void)
{
	LOG_INF("No Wi-Fi credentials: rebooting into the Wi-Fi provisioner");
	if (request_provisioner(BOOT_REQUEST_NO_CREDENTIALS)) {
		reboot_now();
	}
}

void app_prov_identity_update(void)
{
	struct prov_identity id;

	memset(&id, 0, sizeof(id));
	strncpy(id.hostname, app_net_hostname(), sizeof(id.hostname) - 1);
	strncpy(id.service, CONFIG_APP_DNSSD_SERVICE_TYPE, sizeof(id.service) - 1);
	id.port = CONFIG_APP_DNSSD_PORT;
	int rc = prov_identity_store(&id);

	if (rc != 0) {
		LOG_WRN("Could not store the identity record (%d)", rc);
	}
}

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)

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

static void ack_and_reboot(void)
{
	status_led_flash(STATUS_LED_IDENTIFY, ACK_MS);
	k_msleep(ACK_MS);
	reboot_now();
}

static void handle_gesture(enum gesture g)
{
	switch (g) {
	case GESTURE_PROVISION:
		LOG_INF("Button pattern: rebooting into the Wi-Fi provisioner");
		if (request_provisioner(BOOT_REQUEST_OPERATOR)) {
			ack_and_reboot();
		}
		break;
	case GESTURE_ERASE_ARMED:
		LOG_WRN("Erase armed: release the button to erase Wi-Fi credentials");
		status_led_flash(STATUS_LED_ERASE_ARMED, 120000);
		break;
	case GESTURE_ERASE: {
		struct app_wifi_creds c;
		bool have;

		LOG_WRN("Erasing stored Wi-Fi credentials");
		(void)wifi_credentials_delete_all();
		/* Compile-time credentials may still resolve: then the window's
		 * end returns to them rather than waiting. */
		have = app_wifi_creds_resolve(&c) == 0;
		memset(&c, 0, sizeof(c));
		if (request_provisioner(have ? BOOT_REQUEST_OPERATOR
					     : BOOT_REQUEST_NO_CREDENTIALS)) {
			ack_and_reboot();
		}
		reboot_now(); /* nothing to hand over to: restart without them */
	}
	default:
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

void app_prov_button_start(void)
{
	static const struct gesture_cfg cfg = {
		.press_count = CONFIG_APP_WIFI_PROV_PRESS_COUNT,
		.window_ms = CONFIG_APP_WIFI_PROV_PRESS_WINDOW_MS,
		.short_max_ms = 1000,
		.quiet_ms = 700,
		.erase_ms = CONFIG_APP_WIFI_PROV_ERASE_HOLD_S * 1000,
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

void app_prov_button_start(void)
{
}

#endif
