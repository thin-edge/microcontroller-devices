/* SPDX-License-Identifier: Apache-2.0
 *
 * The realtime clock, which TLS needs to judge certificate validity. The
 * application may have set it already (GNSS, a gateway, its own SNTP); the
 * client only asks a time server when it has not.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>
#include <errno.h>
#include <time.h>

#if defined(CONFIG_TEDGE_SNTP)
#include <zephyr/net/sntp.h>
#include <zephyr/net/socketutils.h>
#endif

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

/* Anything before 2025-01-01 means "not set". */
#define CLOCK_FLOOR 1735689600

bool tedge_time_is_valid(void)
{
	struct timespec ts;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return false;
	}
	return ts.tv_sec > CLOCK_FLOOR;
}

int tedge_time_sync(void)
{
#if defined(CONFIG_TEDGE_SNTP)
	struct sntp_time t;
	int rc;

	if (tedge_time_is_valid()) {
		return 0;
	}
	rc = sntp_simple(CONFIG_TEDGE_SNTP_SERVER, 5000, &t);
	if (rc != 0) {
		LOG_WRN("SNTP (%s) failed (%d)", CONFIG_TEDGE_SNTP_SERVER, rc);
		return rc;
	}
	struct timespec ts = { .tv_sec = (time_t)t.seconds, .tv_nsec = 0 };

	rc = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	if (rc != 0) {
		LOG_ERR("could not set the clock (%d)", rc);
		return rc;
	}
	LOG_INF("clock set from %s", CONFIG_TEDGE_SNTP_SERVER);
	return 0;
#else
	if (tedge_time_is_valid()) {
		return 0;
	}
	LOG_WRN("the clock is not set and CONFIG_TEDGE_SNTP is off: the "
		"application must set it before the client can connect");
	return -ENOTSUP;
#endif
}
