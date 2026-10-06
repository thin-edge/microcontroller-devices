/* SPDX-License-Identifier: Apache-2.0 */

#include "identify.h"
#include "net.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <tedge/tedge.h>

LOG_MODULE_REGISTER(app_identify, CONFIG_LOG_DEFAULT_LEVEL);

#define IDENTIFY_MAX_S   300
#define GESTURE_ACK_MS   1000
#define EVENT_MIN_GAP_MS 5000 /* repeated gestures send one event */

void app_identify_gesture(void)
{
	static int64_t last_event;
	static bool sent;
	int64_t now = k_uptime_get();
	char text[96];
	int rc;

	status_led_flash(STATUS_LED_IDENTIFY, GESTURE_ACK_MS);
	if (sent && now - last_event < EVENT_MIN_GAP_MS) {
		LOG_INF("Identify pattern again within %d s: no new event",
			EVENT_MIN_GAP_MS / 1000);
		return;
	}
	snprintf(text, sizeof(text), "Identify button pressed on %s",
		 app_net_hostname());
	/* Only queues the event: the client's thread sends it, after a
	 * reconnect if need be. */
	rc = tedge_publish_event("zephyr_Identify", text, 0);
	if (rc != 0) {
		LOG_WRN("Identify pattern: event not queued (%d)", rc);
		return;
	}
	LOG_INF("Identify pattern: zephyr_Identify event queued");
	sent = true;
	last_event = now;
}

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

/* `tedge identify [seconds]`. Returns as soon as the blinking starts: the
 * cloud runs one command at a time under a timeout that is no longer than
 * the default blink. */
static int cmd_identify(const struct shell *sh, size_t argc, char **argv)
{
	long secs = CONFIG_APP_IDENTIFY_DURATION_S;

	if (argc > 1) {
		char *end;

		secs = strtol(argv[1], &end, 10);
		if (end == argv[1] || *end != '\0' || secs <= 0) {
			shell_error(sh, "the duration must be a whole number of "
					"seconds above 0, not \"%s\"", argv[1]);
			return -EINVAL;
		}
		secs = MIN(secs, IDENTIFY_MAX_S);
	}
	if (!status_led_present()) {
		shell_error(sh, "this board has no status LED");
		return -ENODEV;
	}
	status_led_flash(STATUS_LED_IDENTIFY, (uint32_t)secs * 1000U);
	LOG_INF("Identify: blinking the status LED for %ld s", secs);
	shell_print(sh, "identifying for %ld s", secs);
	return 0;
}

/* Hangs off the client's own "tedge" root, which tedge-zephyr defines. */
SHELL_SUBCMD_ADD((tedge), identify, NULL,
		 "blink the status LED to find this board: identify [seconds]",
		 cmd_identify, 1, 1);
#endif /* CONFIG_SHELL */
