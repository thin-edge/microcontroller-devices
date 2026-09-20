/* SPDX-License-Identifier: Apache-2.0
 *
 * Certificate renewal.
 *
 * A device certificate expires on a date that is already fixed, and a device
 * whose certificate has expired cannot be told anything: no operations, no
 * remote access, no firmware update. So the client watches its own expiry
 * and renews well before it matters:
 *
 *   remaining lifetime < CONFIG_TEDGE_CERT_RENEW_BEFORE_DAYS
 *     -> CSR for the same key, signed inside PSA
 *     -> POST /.well-known/est/simplereenroll with the Bearer token
 *        (the endpoint refuses a client certificate alone)
 *     -> store the new certificate, replace the TLS credential
 *     -> reconnect, so the session uses it
 *
 * A renewal that fails changes nothing: the device keeps its working
 * certificate and tries again. What must not happen is failing quietly, so
 * the expiry is published as twin data and an alarm is raised while there is
 * still time to act.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/timeutil.h>

#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define EST_REENROLL_PATH "/.well-known/est/simplereenroll"
#define SECONDS_PER_DAY   86400
/* While inside the renewal margin, try again this often rather than daily. */
#define RETRY_INSIDE_MARGIN_MS (60 * 60 * 1000)

static int64_t next_check_at;   /* uptime ms */
static time_t cert_expires;     /* 0 until the certificate has been read */
static uint32_t renewals;       /* how many this device has done */
static bool alarm_raised;

/* ------------------------------------------------------------------------ */
/* The certificate's expiry                                                  */
/* ------------------------------------------------------------------------ */

/* mbedTLS gives the validity as a broken-down time; turn it into epoch
 * seconds without pulling in a full timezone database. */
static time_t to_epoch(const mbedtls_x509_time *t)
{
	struct tm tm = {
		.tm_year = t->year - 1900,
		.tm_mon = t->mon - 1,
		.tm_mday = t->day,
		.tm_hour = t->hour,
		.tm_min = t->min,
		.tm_sec = t->sec,
	};

	return (time_t)timeutil_timegm64(&tm);
}

static int read_expiry(time_t *expires)
{
	mbedtls_x509_crt crt;
	const uint8_t *der;
	size_t len = 0;
	int ret;

	der = tedge_credentials_cert(&len);
	if (der == NULL || len == 0) {
		return -ENOENT;
	}
	mbedtls_x509_crt_init(&crt);
	ret = mbedtls_x509_crt_parse_der(&crt, der, len);
	if (ret == 0) {
		*expires = to_epoch(&crt.valid_to);
	} else {
		LOG_WRN("certificate: cannot read the expiry (-0x%04x)", -ret);
		ret = -EBADMSG;
	}
	mbedtls_x509_crt_free(&crt);
	return ret;
}

static int days_remaining(void)
{
	struct timespec now;

	if (cert_expires == 0 || !tedge_time_is_valid()) {
		return -1;
	}
	(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &now);
	return (int)((cert_expires - now.tv_sec) / SECONDS_PER_DAY);
}

int tedge_cert_twin(char *buf, size_t len)
{
	struct tm tm;
	char iso[24] = "unknown";
	int days;

	if (cert_expires == 0 && read_expiry(&cert_expires) != 0) {
		return -ENOENT;
	}
	days = days_remaining(); /* after the expiry is known, not before */
	gmtime_r(&cert_expires, &tm);
	strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return (snprintf(buf, len,
			 "{\"expires\":\"%s\",\"daysRemaining\":%d,"
			 "\"renewals\":%u}",
			 iso, days, renewals) < (int)len)
		       ? 0
		       : -ENOSPC;
}

/* ------------------------------------------------------------------------ */
/* The alarm                                                                 */
/* ------------------------------------------------------------------------ */

static void raise_alarm(int days)
{
	struct tm tm;
	char iso[24];
	char line[160];
	char text[110];
	char quoted[128];

	if (alarm_raised) {
		return;
	}
	gmtime_r(&cert_expires, &tm);
	strftime(iso, sizeof(iso), "%Y-%m-%d", &tm);
	snprintf(text, sizeof(text),
		 "the device certificate expires on %s (%d days) and could not "
		 "be renewed", iso, days);
	(void)tedge_sr_quote(text, quoted, sizeof(quoted));
	snprintf(line, sizeof(line), "301,c8y_CertificateExpiring,%s", quoted);
	if (tedge_c8y_publish_sr(line) == 0) {
		alarm_raised = true;
		LOG_ERR("certificate: %s", text);
	}
}

static void clear_alarm(void)
{
	if (!alarm_raised) {
		return;
	}
	if (tedge_c8y_publish_sr("306,c8y_CertificateExpiring") == 0) {
		alarm_raised = false;
	}
}

/* ------------------------------------------------------------------------ */
/* Renewal                                                                   */
/* ------------------------------------------------------------------------ */

static int renew(void)
{
	static uint8_t new_cert[1024];
	size_t new_len = 0;
	char auth[1100];
	const char *token = tedge_c8y_jwt();
	int64_t t0 = k_uptime_get();
	int ret;

	if (token == NULL || token[0] == '\0') {
		return -EAGAIN; /* no token yet; the next check will try again */
	}

	ret = tedge_est_make_csr(NULL, 0);
	if (ret != 0) {
		LOG_ERR("certificate: could not build the renewal request (%d)",
			ret);
		tedge_est_release();
		return ret;
	}
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);
	ret = tedge_est_request(EST_REENROLL_PATH, auth, new_cert,
				sizeof(new_cert), &new_len);
	memset(auth, 0, sizeof(auth));
	tedge_est_release();
	if (ret != 0) {
		LOG_WRN("certificate: renewal failed (%d); keeping the current "
			"certificate", ret);
		return ret;
	}

	ret = tedge_credentials_replace(new_cert, new_len);
	if (ret != 0) {
		LOG_ERR("certificate: the new certificate could not be stored "
			"(%d); keeping the current one", ret);
		return ret;
	}
	cert_expires = 0;
	(void)read_expiry(&cert_expires);
	renewals++;
	LOG_INF("certificate: renewed in %lld ms (%zu B), now valid for %d days",
		k_uptime_get() - t0, new_len, days_remaining());
	clear_alarm();
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The schedule                                                              */
/* ------------------------------------------------------------------------ */

void tedge_cert_renew_tick(void)
{
	int days;

	if (!tedge_time_is_valid()) {
		return; /* the expiry means nothing without a clock */
	}
	if (next_check_at != 0 && k_uptime_get() < next_check_at) {
		return;
	}
	if (cert_expires == 0 && read_expiry(&cert_expires) != 0) {
		next_check_at = k_uptime_get() +
				CONFIG_TEDGE_CERT_RENEW_CHECK_HOURS * 3600000LL;
		return;
	}

	days = days_remaining();
	if (tedge_cert_action(days, CONFIG_TEDGE_CERT_RENEW_BEFORE_DAYS,
			      CONFIG_TEDGE_CERT_RENEW_ALARM_DAYS,
			      false) == TEDGE_CERT_WAIT) {
		/* Spread the next check within the interval, so a fleet
		 * enrolled together does not renew together. */
		int64_t interval =
			CONFIG_TEDGE_CERT_RENEW_CHECK_HOURS * 3600000LL;

		next_check_at = k_uptime_get() + interval / 2 +
				(sys_rand32_get() % (uint32_t)interval);
		LOG_DBG("certificate: %d days left; next check in %lld min", days,
			(next_check_at - k_uptime_get()) / 60000);
		return;
	}

	LOG_INF("certificate: %d days left; renewing", days);
	if (renew() == 0) {
		next_check_at = k_uptime_get() +
				CONFIG_TEDGE_CERT_RENEW_CHECK_HOURS * 3600000LL;
		/* The session still uses the old certificate: a reconnect
		 * picks up the new one while the old one is still valid. */
		tedge_request_reconnect();
		return;
	}

	next_check_at = k_uptime_get() + RETRY_INSIDE_MARGIN_MS;
	if (tedge_cert_action(days, CONFIG_TEDGE_CERT_RENEW_BEFORE_DAYS,
			      CONFIG_TEDGE_CERT_RENEW_ALARM_DAYS,
			      true) == TEDGE_CERT_ALARM) {
		raise_alarm(days);
	}
}
