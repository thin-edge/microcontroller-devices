/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the module's pure helpers: SmartREST CSV, the reconnect
 * back-off and the PKCS#7 unwrap that reads the enrollment reply.
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

/* The module's sources log under this name; a test build has to register it
 * because the module's own Kconfig is not part of one. */
LOG_MODULE_REGISTER(tedge, 3);

#include "tedge_internal.h"
#include "pkcs7_fixture.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* SmartREST                                                                 */
/* ------------------------------------------------------------------------ */

ZTEST(tedge_smartrest, test_plain_fields)
{
	const char *line = "530,tedge-abc,192.168.1.20,22,key-1234";
	char out[64];

	zassert_equal(tedge_sr_template(line), 530);
	zassert_true(tedge_sr_field(line, 1, out, sizeof(out)) > 0);
	zassert_str_equal(out, "tedge-abc");
	zassert_true(tedge_sr_field(line, 3, out, sizeof(out)) > 0);
	zassert_str_equal(out, "22");
	zassert_true(tedge_sr_field(line, 4, out, sizeof(out)) > 0);
	zassert_str_equal(out, "key-1234");
	zassert_equal(tedge_sr_field(line, 9, out, sizeof(out)), -ENOENT);
}

ZTEST(tedge_smartrest, test_quoted_fields)
{
	/* A quoted field may hold commas, and "" is one quote. */
	const char *line = "502,c8y_Restart,\"not now, the device is \"\"busy\"\"\"";
	char out[64];

	zassert_equal(tedge_sr_template(line), 502);
	zassert_true(tedge_sr_field(line, 1, out, sizeof(out)) > 0);
	zassert_str_equal(out, "c8y_Restart");
	zassert_true(tedge_sr_field(line, 2, out, sizeof(out)) > 0);
	zassert_str_equal(out, "not now, the device is \"busy\"");
}

ZTEST(tedge_smartrest, test_empty_and_short_buffer)
{
	const char *line = "71,,third";
	char out[4];

	zassert_equal(tedge_sr_field(line, 1, out, sizeof(out)), 0);
	zassert_str_equal(out, "");
	/* A field longer than the buffer is truncated, never overrun. */
	zassert_equal(tedge_sr_field(line, 2, out, sizeof(out)), 3);
	zassert_str_equal(out, "thi");
}

ZTEST(tedge_smartrest, test_not_smartrest)
{
	zassert_true(tedge_sr_template("{\"status\":\"up\"}") < 0);
	zassert_true(tedge_sr_template("") < 0);
}

ZTEST(tedge_smartrest, test_quote)
{
	char out[64];

	zassert_true(tedge_sr_quote("plain", out, sizeof(out)) > 0);
	zassert_str_equal(out, "\"plain\"");

	zassert_true(tedge_sr_quote("say \"hi\"", out, sizeof(out)) > 0);
	zassert_str_equal(out, "\"say \"\"hi\"\"\"");

	/* Newlines would end the line: they become spaces. */
	zassert_true(tedge_sr_quote("two\nlines", out, sizeof(out)) > 0);
	zassert_str_equal(out, "\"two lines\"");
}

ZTEST(tedge_smartrest, test_quote_truncates_safely)
{
	char out[8];

	zassert_true(tedge_sr_quote("0123456789", out, sizeof(out)) > 0);
	zassert_equal(strlen(out), 7);
	zassert_equal(out[0], '"');
	zassert_equal(out[6], '"');
}

ZTEST_SUITE(tedge_smartrest, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Back-off                                                                  */
/* ------------------------------------------------------------------------ */

#define MAX_S 300

ZTEST(tedge_backoff, test_starts_at_the_floor_and_doubles)
{
	uint32_t first = tedge_backoff_next(0, MAX_S);

	/* The first wait is the floor exactly: no jitter below it. */
	zassert_equal(first, 3);

	uint32_t second = tedge_backoff_next(first, MAX_S);

	zassert_true(second >= 3 && second <= 8, "second wait was %u", second);
}

ZTEST(tedge_backoff, test_caps_at_the_maximum)
{
	uint32_t max = MAX_S;
	uint32_t wait = 1;

	for (int i = 0; i < 20; i++) {
		wait = tedge_backoff_next(wait, max);
		zassert_true(wait <= max + max / 5,
			     "wait %u exceeded the maximum %u", wait, max);
	}
	/* After enough doublings it sits near the maximum, not below it. */
	zassert_true(wait >= max - max / 5, "wait %u fell back", wait);
}

ZTEST(tedge_backoff, test_jitter_spreads_attempts)
{
	bool differed = false;
	uint32_t base = tedge_backoff_next(tedge_backoff_next(60, MAX_S), MAX_S);

	for (int i = 0; i < 20 && !differed; i++) {
		differed = (tedge_backoff_next(tedge_backoff_next(60, MAX_S),
					       MAX_S) != base);
	}
	zassert_true(differed, "the back-off never varied: no jitter");
}

ZTEST_SUITE(tedge_backoff, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Remote access                                                             */
/* ------------------------------------------------------------------------ */

ZTEST(tedge_remote_access, test_request_fields)
{
	/* 530,<serial>,<host>,<port>,<connectionKey> */
	const char *line = "530,tedge-abc,192.168.68.72,22,"
			   "a1b2c3d4-e5f6-7890-abcd-ef1234567890";
	char host[64], port[8], key[48];

	zassert_equal(tedge_sr_template(line), 530);
	zassert_true(tedge_sr_field(line, 2, host, sizeof(host)) > 0);
	zassert_str_equal(host, "192.168.68.72");
	zassert_true(tedge_sr_field(line, 3, port, sizeof(port)) > 0);
	zassert_str_equal(port, "22");
	zassert_equal(tedge_sr_field(line, 4, key, sizeof(key)), 36);
}

ZTEST(tedge_remote_access, test_allow_list)
{
	const char *list = "192.168.1.20:22,10.0.0.5:502,pi.local:23";

	zassert_true(tedge_ra_in_allow_list(list, "192.168.1.20", 22));
	zassert_true(tedge_ra_in_allow_list(list, "10.0.0.5", 502));
	zassert_true(tedge_ra_in_allow_list(list, "pi.local", 23));

	/* The port is part of the entry. */
	zassert_false(tedge_ra_in_allow_list(list, "192.168.1.20", 23));
	/* A prefix of an entry is not an entry. */
	zassert_false(tedge_ra_in_allow_list(list, "192.168.1.2", 22));
	zassert_false(tedge_ra_in_allow_list(list, "192.168.1.21", 22));
	/* An empty list allows nothing. */
	zassert_false(tedge_ra_in_allow_list("", "192.168.1.20", 22));
}

ZTEST_SUITE(tedge_remote_access, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* URLs and the token rule (firmware update)                                 */
/* ------------------------------------------------------------------------ */

/* tedge_url.c asks the client which tenant it talks to; the test decides. */
static const char *test_tenant = "tedge-dev05.preprod.c8y.io";

const char *tedge_c8y_host(void)
{
	return test_tenant;
}

ZTEST(tedge_url, test_split)
{
	char host[64];
	const char *path;
	uint16_t port;
	bool tls;

	zassert_equal(tedge_url_split("https://example.com/a/b.bin", &tls, host,
				      sizeof(host), &port, &path),
		      0);
	zassert_true(tls);
	zassert_str_equal(host, "example.com");
	zassert_equal(port, 443);
	zassert_str_equal(path, "/a/b.bin");

	zassert_equal(tedge_url_split("http://10.0.0.5:8080/f", &tls, host,
				      sizeof(host), &port, &path),
		      0);
	zassert_false(tls);
	zassert_str_equal(host, "10.0.0.5");
	zassert_equal(port, 8080);

	/* No path means the root. */
	zassert_equal(tedge_url_split("https://example.com", &tls, host,
				      sizeof(host), &port, &path),
		      0);
	zassert_str_equal(path, "/");

	/* Anything else is refused. */
	zassert_true(tedge_url_split("ftp://example.com/x", &tls, host,
				     sizeof(host), &port, &path) < 0);
	zassert_true(tedge_url_split("https:///x", &tls, host, sizeof(host),
				     &port, &path) < 0);
}

ZTEST(tedge_url, test_is_tenant)
{
	/* The tenant itself, and the tenant-ID host Cumulocity serves
	 * binaries from, are both inside the tenant's domain. */
	zassert_true(tedge_url_is_tenant("tedge-dev05.preprod.c8y.io"));
	zassert_true(tedge_url_is_tenant("t297258657.preprod.c8y.io"));

	/* Anywhere else is not, including a look-alike. */
	zassert_false(tedge_url_is_tenant("github.com"));
	zassert_false(tedge_url_is_tenant("release-assets.githubusercontent.com"));
	zassert_false(tedge_url_is_tenant("preprod.c8y.io.evil.example"));
	zassert_false(tedge_url_is_tenant(""));
}

ZTEST(tedge_url, test_resolve_redirect)
{
	const char *base = "https://t1.preprod.c8y.io/inventory/binaries/42";
	char out[256];

	/* Absolute. */
	zassert_equal(tedge_url_resolve(base, "https://cdn.example/x.bin", out,
					sizeof(out)),
		      0);
	zassert_str_equal(out, "https://cdn.example/x.bin");

	/* Root-relative keeps the scheme and host. */
	zassert_equal(tedge_url_resolve(base, "/other/path", out, sizeof(out)),
		      0);
	zassert_str_equal(out, "https://t1.preprod.c8y.io/other/path");

	/* Relative. */
	zassert_equal(tedge_url_resolve(base, "next.bin", out, sizeof(out)), 0);
	zassert_str_equal(out, "https://t1.preprod.c8y.io/next.bin");

	/* An empty target, or one that will not fit, is refused. */
	zassert_true(tedge_url_resolve(base, "", out, sizeof(out)) < 0);
	char tiny[8];

	zassert_true(tedge_url_resolve(base, "https://cdn.example/x.bin", tiny,
				       sizeof(tiny)) < 0);
}

ZTEST_SUITE(tedge_url, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Telemetry: the buffer and the messages                                    */
/* ------------------------------------------------------------------------ */

/* The transport, stubbed: the test decides whether a publish works and
 * records what it was asked to send. */
static bool transport_up;
static int sent_count;
static enum tedge_msg_kind sent_kind[16];
static char sent_type[16][40];
static char sent_payload[16][224];

int tedge_c8y_publish_telemetry(enum tedge_msg_kind kind, const char *type,
				const char *payload)
{
	if (!transport_up) {
		return -ENOTCONN;
	}
	if (sent_count < (int)ARRAY_SIZE(sent_kind)) {
		sent_kind[sent_count] = kind;
		strncpy(sent_type[sent_count], type,
			sizeof(sent_type[0]) - 1);
		strncpy(sent_payload[sent_count], payload,
			sizeof(sent_payload[0]) - 1);
		sent_count++;
	}
	return 0;
}

void tedge_telemetry_wake(void) { }

bool tedge_time_is_valid(void)
{
	return true; /* the tests do not depend on a real clock */
}

void tedge_json_escape(const char *in, char *out, size_t len)
{
	size_t n = 0;

	for (const char *p = in; *p && n + 2 < len; p++) {
		if (*p == '"' || *p == '\\') {
			out[n++] = '\\';
		}
		out[n++] = *p;
	}
	out[n] = '\0';
}

static void telemetry_before(void *fixture)
{
	ARG_UNUSED(fixture);
	transport_up = true;
	sent_count = 0;
	/* Drain anything a previous test left queued. */
	tedge_telemetry_flush();
	sent_count = 0;
}

ZTEST(tedge_telemetry, test_measurement_round_trip)
{
	struct tedge_measurement_value v[] = {
		{ .series = "temperature", .value = 21.5, .unit = "C" },
		{ .series = "humidity", .value = 48.25 },
	};

	zassert_equal(tedge_publish_measurement("environment", v, 2, 0), 0);
	tedge_telemetry_flush();
	zassert_equal(sent_count, 1);
	zassert_equal(sent_kind[0], TEDGE_MSG_MEASUREMENT);
	zassert_str_equal(sent_type[0], "environment");
	zassert_not_null(strstr(sent_payload[0], "\"temperature\":21.50"));
	zassert_not_null(strstr(sent_payload[0], "\"humidity\":48.25"));
	zassert_not_null(strstr(sent_payload[0], "\"time\":\""));
}

ZTEST(tedge_telemetry, test_application_timestamp_is_kept)
{
	struct tedge_measurement_value v = { .series = "x", .value = 1 };

	/* An application that sampled earlier passes its own time, and that
	 * is the time the message must carry. */
	zassert_equal(tedge_publish_measurement("t", &v, 1, 1789891200000LL), 0);
	tedge_telemetry_flush();
	zassert_equal(sent_count, 1);
	zassert_not_null(strstr(sent_payload[0],
				"\"time\":\"2026-09-20T08:00:00Z\""),
			 "payload was %s", sent_payload[0]);
}

ZTEST(tedge_telemetry, test_event_and_alarm)
{
	zassert_equal(tedge_publish_event("boot", "started \"cleanly\"", 0), 0);
	zassert_equal(tedge_raise_alarm("overheating", TEDGE_ALARM_MAJOR,
					"too hot"),
		      0);
	zassert_equal(tedge_clear_alarm("overheating"), 0);
	tedge_telemetry_flush();

	zassert_equal(sent_count, 3);
	zassert_equal(sent_kind[0], TEDGE_MSG_EVENT);
	/* The quotes in the text must not break the payload. */
	zassert_not_null(strstr(sent_payload[0], "started \\\"cleanly\\\""));
	zassert_equal(sent_kind[1], TEDGE_MSG_ALARM);
	zassert_not_null(strstr(sent_payload[1], "\"severity\":\"major\""));
	zassert_equal(sent_kind[2], TEDGE_MSG_ALARM_CLEAR);
	zassert_str_equal(sent_payload[2], "");
}

ZTEST(tedge_telemetry, test_messages_wait_while_offline)
{
	struct tedge_measurement_value v = { .series = "x", .value = 1 };

	transport_up = false;
	zassert_equal(tedge_publish_measurement("t", &v, 1, 0), 0);
	tedge_telemetry_flush();
	zassert_equal(sent_count, 0, "nothing should be sent while offline");

	transport_up = true;
	tedge_telemetry_flush();
	zassert_equal(sent_count, 1, "the message should follow on reconnect");
}

ZTEST(tedge_telemetry, test_full_buffer_drops_oldest_measurement)
{
	struct tedge_measurement_value v = { .series = "x", .value = 1 };
	uint32_t before = tedge_telemetry_dropped();
	int queued = 0;

	transport_up = false;
	/* Fill it well past its size. */
	for (int i = 0; i < 60; i++) {
		if (tedge_publish_measurement("t", &v, 1, 0) == 0) {
			queued++;
		}
	}
	zassert_true(queued > 0);
	zassert_true(tedge_telemetry_dropped() > before,
		     "dropping should be counted");

	transport_up = true;
	tedge_telemetry_flush();
	zassert_true(sent_count > 0, "what is left should still be sent");
}

ZTEST(tedge_telemetry, test_alarms_survive_a_full_buffer)
{
	struct tedge_measurement_value v = { .series = "x", .value = 1 };
	bool found = false;

	transport_up = false;
	zassert_equal(tedge_raise_alarm("fault", TEDGE_ALARM_CRITICAL, "stopped"),
		      0);
	for (int i = 0; i < 60; i++) {
		(void)tedge_publish_measurement("t", &v, 1, 0);
	}
	transport_up = true;
	tedge_telemetry_flush();

	for (int i = 0; i < sent_count; i++) {
		found = found || (sent_kind[i] == TEDGE_MSG_ALARM);
	}
	zassert_true(found, "the alarm must not be dropped for measurements");
}

ZTEST_SUITE(tedge_telemetry, NULL, NULL, telemetry_before, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Certificate renewal                                                       */
/* ------------------------------------------------------------------------ */

#define BEFORE_DAYS 30
#define ALARM_DAYS  7

static enum tedge_cert_action act(int days, bool renewed)
{
	return tedge_cert_action(days, BEFORE_DAYS, ALARM_DAYS, renewed);
}

ZTEST(tedge_cert, test_plenty_of_time)
{
	zassert_equal(act(365, false), TEDGE_CERT_WAIT);
	zassert_equal(act(31, false), TEDGE_CERT_WAIT);
}

ZTEST(tedge_cert, test_inside_the_margin)
{
	zassert_equal(act(30, false), TEDGE_CERT_RENEW);
	zassert_equal(act(8, false), TEDGE_CERT_RENEW);
	/* Expiring today still means renew, not give up. */
	zassert_equal(act(0, false), TEDGE_CERT_RENEW);
}

ZTEST(tedge_cert, test_after_a_failed_attempt)
{
	/* Inside the margin but not yet urgent: try again quietly. */
	zassert_equal(act(20, true), TEDGE_CERT_RETRY);
	/* Close to expiry and still failing: tell the operator. */
	zassert_equal(act(7, true), TEDGE_CERT_ALARM);
	zassert_equal(act(0, true), TEDGE_CERT_ALARM);
}

ZTEST(tedge_cert, test_nothing_to_judge)
{
	/* No clock yet, or no certificate: decide nothing. */
	zassert_equal(act(-1, false), TEDGE_CERT_WAIT);
	zassert_equal(act(-1, true), TEDGE_CERT_WAIT);
}

ZTEST(tedge_cert, test_alarm_can_be_disabled)
{
	zassert_equal(tedge_cert_action(2, BEFORE_DAYS, 0, true),
		      TEDGE_CERT_RETRY);
}

ZTEST_SUITE(tedge_cert, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* PKCS#7 (the enrollment reply)                                             */
/* ------------------------------------------------------------------------ */

ZTEST(tedge_pkcs7, test_takes_the_certificate_out)
{
	static char clean[2048];
	static uint8_t der[1536];
	static uint8_t cert[1024];
	size_t len = 0;

	zassert_equal(tedge_pkcs7_first_cert(pkcs7_b64, clean, sizeof(clean),
					     der, sizeof(der), cert,
					     sizeof(cert), &len),
		      0);
	zassert_equal(len, PKCS7_CERT_DER_LEN, "certificate was %zu B", len);
	zassert_mem_equal(cert, pkcs7_cert_head, sizeof(pkcs7_cert_head));
}

ZTEST(tedge_pkcs7, test_rejects_rubbish)
{
	static char clean[512];
	static uint8_t der[512];
	uint8_t cert[64];
	size_t len = 0;

	zassert_true(tedge_pkcs7_first_cert("not base64 at all!", clean,
					    sizeof(clean), der, sizeof(der),
					    cert, sizeof(cert), &len) < 0);
	/* Valid base64, but not a PKCS#7 structure. */
	zassert_true(tedge_pkcs7_first_cert("aGVsbG8gd29ybGQ=", clean,
					    sizeof(clean), der, sizeof(der),
					    cert, sizeof(cert), &len) < 0);
}

ZTEST(tedge_pkcs7, test_output_buffer_too_small)
{
	static char clean[2048];
	static uint8_t der[1536];
	uint8_t cert[16];
	size_t len = 0;

	zassert_equal(tedge_pkcs7_first_cert(pkcs7_b64, clean, sizeof(clean),
					     der, sizeof(der), cert,
					     sizeof(cert), &len),
		      -ENOMEM);
}

ZTEST_SUITE(tedge_pkcs7, NULL, NULL, NULL, NULL, NULL);
