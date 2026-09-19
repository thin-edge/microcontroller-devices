/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike A, task 3.1: set the realtime clock from SNTP before any TLS connect.
 * mbedTLS checks certificate validity against time(), which reads this clock.
 */

#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/clock.h>

#include "spike.h"

LOG_MODULE_REGISTER(spike_time, LOG_LEVEL_INF);

void spike_time_sync(void)
{
	struct sntp_time ts;
	int attempt = 0;
	int ret;

	for (;;) {
		int64_t t0 = k_uptime_get();

		attempt++;
		ret = sntp_simple(CONFIG_SPIKE_SNTP_SERVER, 5000, &ts);
		if (ret == 0) {
			struct timespec tspec = {
				.tv_sec = (time_t)ts.seconds,
				.tv_nsec = (long)(((uint64_t)ts.fraction * 1000000000ULL) >> 32),
			};
			struct tm tm;

			sys_clock_settime(SYS_CLOCK_REALTIME, &tspec);
			gmtime_r(&tspec.tv_sec, &tm);
			LOG_INF("time set from %s (attempt %d, %lld ms): "
				"%04d-%02d-%02dT%02d:%02d:%02dZ",
				CONFIG_SPIKE_SNTP_SERVER, attempt,
				k_uptime_get() - t0, tm.tm_year + 1900,
				tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
				tm.tm_sec);
			return;
		}
		LOG_WRN("SNTP attempt %d failed (%d); not connecting until the "
			"clock is set", attempt, ret);
		k_sleep(K_SECONDS(MIN(2 * attempt, 30)));
	}
}
