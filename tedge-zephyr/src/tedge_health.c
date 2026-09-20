/* SPDX-License-Identifier: Apache-2.0
 *
 * The client's own vital signs, so that a device which is degrading can be
 * spotted before it fails: how long it has been running, how much of its
 * heap is left, how much telemetry it had to drop, and why it last reset.
 *
 * Deliberately only what the client knows about itself. What an application
 * knows — a sensor, a bus, a Wi-Fi signal, a battery — is the application's
 * to publish, because only it can say what those numbers mean.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>

#include <stdio.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

static int64_t next_at;
static uint32_t reset_cause;
static bool cause_read;

static double reset_reason_code(void)
{
	if (!cause_read) {
		cause_read = true;
		if (hwinfo_get_reset_cause(&reset_cause) != 0) {
			reset_cause = 0;
		}
		(void)hwinfo_clear_reset_cause();
	}
	return (double)reset_cause;
}

void tedge_health_tick(void)
{
	struct tedge_measurement_value values[] = {
		{ .series = "uptime", .unit = "s" },
		{ .series = "freeHeap", .unit = "B" },
		{ .series = "droppedMessages" },
		{ .series = "resetCause" },
	};

	if (next_at != 0 && k_uptime_get() < next_at) {
		return;
	}
	next_at = k_uptime_get() + CONFIG_TEDGE_HEALTH_INTERVAL_S * 1000LL;

	values[0].value = (double)(k_uptime_get() / 1000);
	values[1].value = (double)tedge_heap_free();
	values[2].value = (double)tedge_telemetry_dropped();
	values[3].value = reset_reason_code();
	(void)tedge_publish_measurement("tedge_health", values,
					ARRAY_SIZE(values), 0);
}
