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

/* ------------------------------------------------------------------------ */
/* The shell command's allow-list                                            */
/* ------------------------------------------------------------------------ */

#define ALLOW "kernel uptime, net iface,tedge diag"

static bool allowed(const char *list, const char *cmd)
{
	const char *why = NULL;
	bool ok = tedge_shell_command_allowed(list, cmd, &why);

	zassert_not_null(why, "a decision always explains itself");
	if (!ok) {
		zassert_true(strlen(why) > 0, "a refusal states a reason");
	}
	return ok;
}

ZTEST(tedge_shell_allow, test_empty_list_refuses_everything)
{
	zassert_false(allowed("", "kernel uptime"));
	zassert_false(allowed(NULL, "kernel uptime"));
	zassert_false(allowed("   ", "kernel uptime"));
}

ZTEST(tedge_shell_allow, test_listed_commands_run)
{
	zassert_true(allowed(ALLOW, "kernel uptime"));
	zassert_true(allowed(ALLOW, "tedge diag"));
	/* An entry written with a space after the comma still matches. */
	zassert_true(allowed(ALLOW, "net iface"));
}

ZTEST(tedge_shell_allow, test_arguments_are_allowed)
{
	zassert_true(allowed(ALLOW, "net iface show 1"));
	/* ...but only as arguments, not as a longer command name. */
	zassert_false(allowed(ALLOW, "net ifaceother"));
}

ZTEST(tedge_shell_allow, test_unlisted_command_refused)
{
	zassert_false(allowed(ALLOW, "kernel reboot cold"));
	zassert_false(allowed(ALLOW, "flash erase"));
	zassert_false(allowed(ALLOW, ""));
}

ZTEST(tedge_shell_allow, test_no_smuggling_a_second_command)
{
	zassert_false(allowed(ALLOW, "kernel uptime; kernel reboot cold"));
	zassert_false(allowed(ALLOW, "kernel uptime && flash erase"));
	zassert_false(allowed(ALLOW, "kernel uptime | tee x"));
	zassert_false(allowed(ALLOW, "kernel uptime > /dev/null"));
	zassert_false(allowed(ALLOW, "kernel uptime\nflash erase"));
	zassert_false(allowed(ALLOW, "kernel uptime `flash erase`"));
	/* Even a list that allows everything cannot allow chaining. */
	zassert_false(allowed("kernel", "kernel uptime; kernel reboot cold"));
}

ZTEST(tedge_shell_allow, test_help_is_recognised)
{
	zassert_true(tedge_shell_is_help("help"));
	zassert_true(tedge_shell_is_help("  help  "));
	zassert_true(tedge_shell_is_help("?"));
	zassert_false(tedge_shell_is_help("help kernel"));
	zassert_false(tedge_shell_is_help("helpme"));
	zassert_false(tedge_shell_is_help(""));
	zassert_false(tedge_shell_is_help(NULL));
}

ZTEST(tedge_shell_allow, test_help_lists_the_allow_list)
{
	char out[160];

	zassert_equal(tedge_shell_help_text("kernel uptime, net iface ,tedge diag",
					    out, sizeof(out)), 3);
	zassert_str_equal(out, "Commands this device runs (arguments may follow):"
			       "\n  kernel uptime\n  net iface\n  tedge diag");
}

ZTEST(tedge_shell_allow, test_help_with_an_empty_list)
{
	char out[160];

	zassert_equal(tedge_shell_help_text("", out, sizeof(out)), 0);
	zassert_not_null(strstr(out, "runs no commands"));
	zassert_equal(tedge_shell_help_text(NULL, out, sizeof(out)), 0);
}

ZTEST(tedge_shell_allow, test_help_that_does_not_fit)
{
	char out[40];

	zassert_equal(tedge_shell_help_text("kernel uptime,net iface,tedge diag",
					    out, sizeof(out)), -ENOSPC);
	zassert_true(strlen(out) < sizeof(out));
}

ZTEST_SUITE(tedge_shell_allow, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* The client's own log ring                                                 */
/* ------------------------------------------------------------------------ */

static void ring_before(void *fixture)
{
	uint8_t drain[8];

	ARG_UNUSED(fixture);
	/* There is no "empty it" call, and there should not be: the ring is
	 * written by the logging subsystem. Filling it with whole lines
	 * leaves a known state instead. */
	for (int i = 0; i < 40; i++) {
		tedge_log_ring_write((const uint8_t *)"x\n", 2);
	}
	(void)tedge_log_ring_read(0, drain, sizeof(drain));
}

static size_t ring_text(char *out, size_t len)
{
	size_t n = tedge_log_ring_read(0, (uint8_t *)out, len - 1);

	out[n] = '\0';
	return n;
}

ZTEST(tedge_log_ring, test_lines_come_back_in_order)
{
	char text[300];

	tedge_log_ring_write((const uint8_t *)"first\n", 6);
	tedge_log_ring_write((const uint8_t *)"second\n", 7);
	(void)ring_text(text, sizeof(text));
	zassert_not_null(strstr(text, "first\nsecond\n"),
			 "the ring keeps the order lines arrived in: %s", text);
}

ZTEST(tedge_log_ring, test_it_never_grows_past_its_size)
{
	for (int i = 0; i < 100; i++) {
		tedge_log_ring_write((const uint8_t *)"a line of log\n", 14);
	}
	zassert_true(tedge_log_ring_size() <= 256,
		     "the ring stays inside CONFIG_TEDGE_LOG_RING_BYTES");
}

ZTEST(tedge_log_ring, test_oldest_whole_lines_are_dropped)
{
	char text[300];

	/* More than the ring holds, so the early lines have to go. */
	for (int i = 0; i < 100; i++) {
		tedge_log_ring_write((const uint8_t *)"old\n", 4);
	}
	tedge_log_ring_write((const uint8_t *)"newest\n", 7);
	(void)ring_text(text, sizeof(text));
	zassert_not_null(strstr(text, "newest\n"), "the newest line survives");
	/* What is left starts at a line boundary, never mid-line. */
	zassert_true(text[0] == 'o' || text[0] == 'n',
		     "what is left starts at a line, not mid-line: %s", text);
	zassert_is_null(strstr(text, "x\n"),
			"the lines from before were dropped: %s", text);
}

ZTEST(tedge_log_ring, test_dropping_is_counted)
{
	uint32_t before = tedge_log_ring_dropped();

	for (int i = 0; i < 50; i++) {
		tedge_log_ring_write((const uint8_t *)"filling it up\n", 14);
	}
	zassert_true(tedge_log_ring_dropped() > before,
		     "a ring that overflowed says how many lines it lost");
}

ZTEST(tedge_log_ring, test_a_line_longer_than_the_ring_keeps_its_tail)
{
	char big[400];
	char text[300];

	memset(big, 'z', sizeof(big));
	memcpy(big + sizeof(big) - 5, "tail\n", 5);
	tedge_log_ring_write((const uint8_t *)big, sizeof(big));
	(void)ring_text(text, sizeof(text));
	zassert_not_null(strstr(text, "tail\n"),
			 "the end of an enormous line is the part worth keeping");
}

ZTEST_SUITE(tedge_log_ring, NULL, NULL, ring_before, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Operations delivered as JSON                                              */
/* ------------------------------------------------------------------------ */

/* Shaped like what devicecontrol/notifications delivers, delivery log and
 * all, so the test exercises what the device actually reads. */
#define OP_HEAD                                                               \
	"{\"delivery\":{\"time\":\"2026-09-20T10:00:00.000Z\","               \
	"\"status\":\"SEND\",\"log\":[]},\"agentId\":\"79211726\","           \
	"\"creationTime\":\"2026-09-20T10:00:00.000Z\","                      \
	"\"deviceId\":\"79211726\",\"id\":\"214925\",\"status\":\"PENDING\","

static void translate(const char *json, char *line, size_t len)
{
	zassert_true(tedge_operation_from_json(json, line, len),
		     "an operation with an id is always answerable");
}

ZTEST(tedge_op_json, test_restart_carries_only_its_id)
{
	char line[256];

	translate(OP_HEAD "\"c8y_Restart\":{}}", line, sizeof(line));
	zassert_str_equal(line, "510,214925", "got: %s", line);
}

ZTEST(tedge_op_json, test_command_text)
{
	char line[256];
	char field[64];

	translate(OP_HEAD "\"c8y_Command\":{\"text\":\"kernel uptime\"}}", line,
		  sizeof(line));
	zassert_equal(tedge_sr_template(line), 511, "got: %s", line);
	(void)tedge_sr_field(line, 1, field, sizeof(field));
	zassert_str_equal(field, "214925", "the id takes the serial's place");
	(void)tedge_sr_field(line, 2, field, sizeof(field));
	zassert_str_equal(field, "kernel uptime");
}

ZTEST(tedge_op_json, test_a_comma_in_a_command_survives)
{
	char line[256];
	char field[64];

	translate(OP_HEAD "\"c8y_Command\":{\"text\":\"net iface, please\"}}",
		  line, sizeof(line));
	(void)tedge_sr_field(line, 2, field, sizeof(field));
	zassert_str_equal(field, "net iface, please",
			  "a quoted field keeps its comma: %s", line);
}

ZTEST(tedge_op_json, test_firmware_fields_in_order)
{
	char line[320];
	char field[80];

	translate(OP_HEAD "\"c8y_Firmware\":{\"name\":\"app\","
			  "\"version\":\"1.2.3\",\"url\":\"https://x/y\"}}",
		  line, sizeof(line));
	zassert_equal(tedge_sr_template(line), 515, "got: %s", line);
	(void)tedge_sr_field(line, 2, field, sizeof(field));
	zassert_str_equal(field, "app");
	(void)tedge_sr_field(line, 3, field, sizeof(field));
	zassert_str_equal(field, "1.2.3");
	(void)tedge_sr_field(line, 4, field, sizeof(field));
	zassert_str_equal(field, "https://x/y");
}

ZTEST(tedge_op_json, test_numbers_are_read_as_well_as_strings)
{
	char line[320];
	char field[48];

	translate(OP_HEAD "\"c8y_RemoteAccessConnect\":{\"hostname\":\"10.0.0.5\","
			  "\"port\":22,\"connectionKey\":\"abc\"}}",
		  line, sizeof(line));
	zassert_equal(tedge_sr_template(line), 530, "got: %s", line);
	(void)tedge_sr_field(line, 3, field, sizeof(field));
	zassert_str_equal(field, "22", "a port is a number in JSON");
}

ZTEST(tedge_op_json, test_log_request_keeps_its_filters)
{
	char line[384];
	char field[48];

	translate(OP_HEAD "\"c8y_LogfileRequest\":{\"logFile\":\"tedge-log\","
			  "\"dateFrom\":\"2026-09-20T00:00:00.000Z\","
			  "\"dateTo\":\"2026-09-21T00:00:00.000Z\","
			  "\"searchText\":\"tedge\",\"maximumLines\":5}}",
		  line, sizeof(line));
	zassert_equal(tedge_sr_template(line), 522, "got: %s", line);
	(void)tedge_sr_field(line, 2, field, sizeof(field));
	zassert_str_equal(field, "tedge-log");
	(void)tedge_sr_field(line, 5, field, sizeof(field));
	zassert_str_equal(field, "tedge", "the search text");
	(void)tedge_sr_field(line, 6, field, sizeof(field));
	zassert_str_equal(field, "5", "the line limit");
}

ZTEST(tedge_op_json, test_an_unknown_operation_is_still_answerable)
{
	char line[256];
	char field[48];

	translate(OP_HEAD "\"c8y_SoftwareUpdate\":[{\"name\":\"x\"}]}", line,
		  sizeof(line));
	zassert_equal(tedge_sr_template(line), 599,
		      "an operation with no handler still carries its id: %s",
		      line);
	(void)tedge_sr_field(line, 1, field, sizeof(field));
	zassert_str_equal(field, "214925");
}

ZTEST(tedge_op_json, test_something_that_is_not_an_operation)
{
	char line[256];

	zassert_false(tedge_operation_from_json("{\"status\":\"ok\"}", line,
						sizeof(line)),
		      "no id, nothing to answer");
}

/* A parameter change is the odd one out: its fragment is named after the
 * set, and what it carries is an object rather than a few scalars. */

/* The shape Cumulocity actually sends, captured from operation 214989 on a
 * real device: the "c8y_ParameterUpdate_<set>" fragment is an empty marker
 * naming the set, and the values are a separate top-level fragment named
 * after it, carrying the whole set rather than only what changed. */
ZTEST(tedge_op_json, test_the_shape_cumulocity_really_sends)
{
	const char *json =
		"{\"id\":\"214989\",\"deviceId\":\"79211726\","
		"\"status\":\"PENDING\","
		"\"tedge\":{\"required_interval_min\":30,\"log_level\":\"dbg\","
		"\"health_interval_s\":900},"
		"\"description\":\"Update parameter 'tedge'\","
		"\"c8y_ParameterUpdate\":{},\"c8y_ParameterUpdate_tedge\":{}}";
	char line[512];
	char set[40];
	char object[256];

	translate(json, line, sizeof(line));

	zassert_equal(tedge_sr_template(line), 532, "got: %s", line);
	zassert_true(tedge_sr_field(line, 2, set, sizeof(set)) > 0);
	zassert_str_equal(set, "tedge",
			  "the set comes from the marker's suffix");
	zassert_true(tedge_sr_field(line, 3, object, sizeof(object)) > 0);
	zassert_str_equal(object,
			  "{\"required_interval_min\":30,\"log_level\":\"dbg\","
			  "\"health_interval_s\":900}",
			  "the values come from the fragment named after the "
			  "set, not from the empty marker: %s", object);
}

ZTEST(tedge_op_json, test_an_empty_marker_is_not_mistaken_for_the_values)
{
	const char *json =
		"{\"id\":\"1\",\"c8y_ParameterUpdate_pump\":{},"
		"\"pump\":{\"interval_s\":45}}";
	char line[512];
	char object[128];

	translate(json, line, sizeof(line));
	zassert_true(tedge_sr_field(line, 3, object, sizeof(object)) > 0);
	zassert_str_equal(object, "{\"interval_s\":45}",
			  "the marker comes first in the document: %s", object);
}

/* An operation built by hand through the REST API puts the values inside
 * the suffixed fragment; that is how the hardware verification drove every
 * change, so it stays supported. */
ZTEST(tedge_op_json, test_a_parameter_change_names_its_set)
{
	char line[512];
	char set[40];
	char object[256];

	translate(OP_HEAD
		  "\"c8y_ParameterUpdate_pump\":{\"interval_s\":60,"
		  "\"auto_mode\":false}}",
		  line, sizeof(line));

	zassert_equal(tedge_sr_template(line), 532, "got: %s", line);
	zassert_true(tedge_sr_field(line, 1, set, sizeof(set)) > 0);
	zassert_str_equal(set, "214925", "the id comes first, as always");
	zassert_true(tedge_sr_field(line, 2, set, sizeof(set)) > 0);
	zassert_str_equal(set, "pump", "the set is the fragment's suffix");
	zassert_true(tedge_sr_field(line, 3, object, sizeof(object)) > 0);
	zassert_str_equal(object,
			  "{\"interval_s\":60,\"auto_mode\":false}",
			  "the object survives the line whole: %s", object);
}

ZTEST(tedge_op_json, test_a_parameter_change_keeps_its_strings)
{
	char line[512];
	char object[256];

	translate(OP_HEAD
		  "\"c8y_ParameterUpdate_pump\":{\"site\":\"the shed\"}}",
		  line, sizeof(line));

	zassert_true(tedge_sr_field(line, 3, object, sizeof(object)) > 0);
	zassert_str_equal(object, "{\"site\":\"the shed\"}",
			  "quotes survive being a SmartREST field: %s", object);
}

ZTEST(tedge_op_json, test_a_brace_inside_a_string_does_not_end_the_object)
{
	char line[512];
	char object[256];

	translate(OP_HEAD
		  "\"c8y_ParameterUpdate_pump\":{\"site\":\"a } brace\","
		  "\"interval_s\":9},\"status\":\"PENDING\"}",
		  line, sizeof(line));

	zassert_true(tedge_sr_field(line, 3, object, sizeof(object)) > 0);
	zassert_str_equal(object,
			  "{\"site\":\"a } brace\",\"interval_s\":9}",
			  "got: %s", object);
}

ZTEST(tedge_op_json, test_another_set_is_another_name)
{
	char line[512];
	char set[40];

	translate(OP_HEAD "\"c8y_ParameterUpdate_boiler\":{\"on\":true}}", line,
		  sizeof(line));
	zassert_true(tedge_sr_field(line, 2, set, sizeof(set)) > 0);
	zassert_str_equal(set, "boiler");
}

ZTEST_SUITE(tedge_op_json, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Reading a JSON value that may be a number                                 */
/* ------------------------------------------------------------------------ */

ZTEST(tedge_json_value, test_strings_and_numbers)
{
	const char *json = "{\"a\":\"text\",\"b\":22,\"c\":true,\"d\":1.5}";
	char out[32];

	zassert_true(tedge_json_value(json, "a", out, sizeof(out)) > 0);
	zassert_str_equal(out, "text");
	zassert_true(tedge_json_value(json, "b", out, sizeof(out)) > 0);
	zassert_str_equal(out, "22");
	zassert_true(tedge_json_value(json, "c", out, sizeof(out)) > 0);
	zassert_str_equal(out, "true");
	zassert_true(tedge_json_value(json, "d", out, sizeof(out)) > 0);
	zassert_str_equal(out, "1.5");
}

ZTEST(tedge_json_value, test_a_missing_key_says_so)
{
	char out[32];

	zassert_equal(tedge_json_value("{\"a\":1}", "b", out, sizeof(out)),
		      -ENOENT);
	zassert_str_equal(out, "", "and leaves nothing behind");
}

ZTEST(tedge_json_value, test_the_search_can_start_inside_a_fragment)
{
	const char *json = "{\"port\":1,\"frag\":{\"port\":22}}";
	const char *frag = strstr(json, "\"frag\"");
	char out[16];

	zassert_not_null(frag);
	(void)tedge_json_value(frag, "port", out, sizeof(out));
	zassert_str_equal(out, "22",
			  "a fragment's field, not the one above it");
}

ZTEST_SUITE(tedge_json_value, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Parameters                                                                */
/*                                                                           */
/* tedge_parameters.c is not a pure helper: it allocates, it stores and it   */
/* publishes. Those three are stubbed here rather than mocked away, because  */
/* what the tests are really checking is that the three places which have to */
/* agree — the values, the settings keys and the twin — actually do.         */
/* ------------------------------------------------------------------------ */

#include <zephyr/settings/settings.h>
#include <zephyr/types.h>
#include <stdlib.h>

/* The module heap. */
void *tedge_alloc(size_t size)
{
	return malloc(size);
}

void tedge_free(void *p)
{
	free(p);
}

/* The twin, stubbed: the last thing published, per fragment. */
static char twin_fragment[40];
static char twin_json[512];
static int twin_publishes;

int tedge_publish_twin(const char *fragment, const char *json)
{
	strncpy(twin_fragment, fragment, sizeof(twin_fragment) - 1);
	strncpy(twin_json, json != NULL ? json : "", sizeof(twin_json) - 1);
	twin_publishes++;
	return 0;
}

/* Settings, stubbed: a flat key/value store, so a test can see exactly
 * which keys were written and prove that an unchanged value wrote none. */
#define STORE_SLOTS 16
static struct {
	char key[64];
	uint8_t value[64];
	size_t len;
	bool used;
} store_slot[STORE_SLOTS];
static int store_writes;

static void store_clear(void)
{
	memset(store_slot, 0, sizeof(store_slot));
	store_writes = 0;
}

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	int free_slot = -1;

	if (val_len > sizeof(store_slot[0].value)) {
		return -ENOMEM;
	}
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (store_slot[i].used && strcmp(store_slot[i].key, name) == 0) {
			free_slot = i;
			break;
		}
		if (!store_slot[i].used && free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot < 0) {
		return -ENOMEM;
	}
	strncpy(store_slot[free_slot].key, name,
		sizeof(store_slot[0].key) - 1);
	memcpy(store_slot[free_slot].value, value, val_len);
	store_slot[free_slot].len = val_len;
	store_slot[free_slot].used = true;
	store_writes++;
	return 0;
}

int settings_delete(const char *name)
{
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (store_slot[i].used && strcmp(store_slot[i].key, name) == 0) {
			store_slot[i].used = false;
			return 0;
		}
	}
	return 0;
}

static ssize_t store_read(void *cb_arg, void *data, size_t len)
{
	size_t i = (size_t)(uintptr_t)cb_arg;
	size_t n = MIN(len, store_slot[i].len);

	memcpy(data, store_slot[i].value, n);
	return (ssize_t)n;
}

int settings_load_subtree_direct(const char *subtree,
				 settings_load_direct_cb cb, void *param)
{
	size_t prefix = strlen(subtree);

	for (size_t i = 0; i < STORE_SLOTS; i++) {
		const char *key = store_slot[i].key;
		const char *relative;

		if (!store_slot[i].used ||
		    strncmp(key, subtree, prefix) != 0) {
			continue;
		}
		if (key[prefix] == '\0') {
			relative = ""; /* the subtree is the value itself */
		} else if (key[prefix] == '/') {
			relative = key + prefix + 1;
		} else {
			continue; /* "tedge/paramX" is not under "tedge/param" */
		}
		(void)cb(relative, store_slot[i].len, store_read,
			 (void *)(uintptr_t)i, param);
	}
	return 0;
}

/* The set under test: one parameter of every type, so a single declaration
 * covers validation, the twin and the schema. */
static const struct tedge_parameter pump_params[] = {
	TEDGE_PARAM_INT("interval_s", 30, 5, 3600, "Seconds between reads"),
	TEDGE_PARAM_BOOL("auto_mode", true, "Run the pump automatically"),
	TEDGE_PARAM_ENUM("profile", "normal", ("normal", "quiet", "boost"),
			 "Operating profile. Quiet trades flow for noise at night; boost run"
			 "s past the rated speed for a short while, and is logged as it does"),
	TEDGE_PARAM_STRING("site", "", 8, "Where this device is"),
};

/* What the application's hook will do, and what it saw. */
static int hook_verdict;
static int hook_calls;

static int pump_changed(const char *set, char *reason, size_t reason_len,
			void *user_data)
{
	ARG_UNUSED(set);
	ARG_UNUSED(user_data);
	hook_calls++;
	if (hook_verdict != 0) {
		snprintf(reason, reason_len, "the pump is running");
	}
	return hook_verdict;
}

static void *params_setup(void)
{
	static bool declared_once;

	if (!declared_once) {
		zassert_ok(tedge_declare_parameters("pump", pump_params,
						    ARRAY_SIZE(pump_params),
						    pump_changed, NULL));
		declared_once = true;
	}
	return NULL;
}

/* Every test starts from the declared defaults, an empty store and a hook
 * that agrees. */
static void params_before(void *fixture)
{
	char reason[128];

	ARG_UNUSED(fixture);
	hook_verdict = 0;
	hook_calls = 0;
	store_clear();
	twin_publishes = 0;
	(void)tedge_params_apply("pump",
				 "{\"interval_s\":30,\"auto_mode\":true,"
				 "\"profile\":\"normal\",\"site\":\"\"}",
				 reason, sizeof(reason));
	store_clear();
	twin_publishes = 0;
	hook_calls = 0;
}

static int apply(const char *json, char *reason, size_t len)
{
	return tedge_params_apply("pump", json, reason, len);
}

/* --- Validation ---------------------------------------------------------- */

ZTEST(tedge_parameters, test_each_type_round_trips)
{
	char reason[128];
	char site[16];
	int32_t interval;
	bool automatic;

	zassert_ok(apply("{\"interval_s\":60,\"auto_mode\":false,"
			 "\"profile\":\"quiet\",\"site\":\"shed\"}",
			 reason, sizeof(reason)));

	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 60);
	zassert_ok(tedge_parameter_get_bool("pump", "auto_mode", &automatic));
	zassert_false(automatic);
	zassert_ok(tedge_parameter_get_string("pump", "site", site,
					      sizeof(site)));
	zassert_str_equal(site, "shed");
	zassert_ok(tedge_parameter_get_string("pump", "profile", site,
					      sizeof(site)));
	zassert_str_equal(site, "quiet", "an enum reads as the string it is");
}

ZTEST(tedge_parameters, test_the_edges_of_a_range_are_inside_it)
{
	char reason[128];
	int32_t interval;

	zassert_ok(apply("{\"interval_s\":5}", reason, sizeof(reason)));
	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 5, "the minimum is allowed");

	zassert_ok(apply("{\"interval_s\":3600}", reason, sizeof(reason)));
	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 3600, "and so is the maximum");
}

ZTEST(tedge_parameters, test_a_value_below_its_range_is_refused)
{
	char reason[128];
	int32_t interval;

	zassert_equal(apply("{\"interval_s\":4}", reason, sizeof(reason)),
		      -ERANGE);
	zassert_not_null(strstr(reason, "interval_s"),
			 "the refusal names the parameter: %s", reason);
	zassert_not_null(strstr(reason, "5..3600"),
			 "and the limit it broke: %s", reason);
	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 30, "and nothing changed");
}

ZTEST(tedge_parameters, test_a_value_above_its_range_is_refused)
{
	char reason[128];

	zassert_equal(apply("{\"interval_s\":3601}", reason, sizeof(reason)),
		      -ERANGE);
	zassert_not_null(strstr(reason, "interval_s"), "%s", reason);
}

ZTEST(tedge_parameters, test_a_number_that_is_not_one_is_refused)
{
	char reason[128];

	zassert_equal(apply("{\"interval_s\":\"soon\"}", reason,
			    sizeof(reason)),
		      -EINVAL);
	zassert_not_null(strstr(reason, "whole number"), "%s", reason);
}

ZTEST(tedge_parameters, test_a_boolean_only_takes_true_or_false)
{
	char reason[128];

	zassert_equal(apply("{\"auto_mode\":\"yes\"}", reason, sizeof(reason)),
		      -EINVAL);
	zassert_not_null(strstr(reason, "auto_mode"), "%s", reason);
	zassert_not_null(strstr(reason, "true or false"), "%s", reason);
}

ZTEST(tedge_parameters, test_a_string_longer_than_declared_is_refused)
{
	char reason[128];

	zassert_equal(apply("{\"site\":\"a very long place indeed\"}", reason,
			    sizeof(reason)),
		      -EINVAL);
	zassert_not_null(strstr(reason, "site"), "%s", reason);
	zassert_not_null(strstr(reason, "at most 8"), "%s", reason);
}

ZTEST(tedge_parameters, test_an_enum_only_takes_what_it_declared)
{
	char reason[128];
	char profile[16];

	zassert_equal(apply("{\"profile\":\"turbo\"}", reason, sizeof(reason)),
		      -EINVAL);
	zassert_not_null(strstr(reason, "profile"), "%s", reason);
	zassert_not_null(strstr(reason, "normal"),
			 "the refusal lists what is allowed: %s", reason);
	zassert_ok(tedge_parameter_get_string("pump", "profile", profile,
					      sizeof(profile)));
	zassert_str_equal(profile, "normal");
}

ZTEST(tedge_parameters, test_a_name_nobody_declared_is_refused)
{
	char reason[128];

	zassert_equal(apply("{\"interval_s\":60,\"colour\":\"red\"}", reason,
			    sizeof(reason)),
		      -ENOENT);
	zassert_not_null(strstr(reason, "colour"),
			 "the refusal names the name it does not know: %s",
			 reason);
}

ZTEST(tedge_parameters, test_a_set_nobody_declared_is_refused)
{
	char reason[128];

	zassert_equal(tedge_params_apply("boiler", "{\"interval_s\":60}",
					 reason, sizeof(reason)),
		      -ENOENT);
	zassert_not_null(strstr(reason, "boiler"), "%s", reason);
}

ZTEST(tedge_parameters, test_a_change_is_all_or_nothing)
{
	char reason[128];
	int32_t interval;
	char profile[16];

	/* The good value comes first, the bad one after it. */
	zassert_equal(apply("{\"interval_s\":60,\"profile\":\"turbo\"}", reason,
			    sizeof(reason)),
		      -EINVAL);
	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 30,
		      "the value before the bad one was not applied either");
	zassert_ok(tedge_parameter_get_string("pump", "profile", profile,
					      sizeof(profile)));
	zassert_str_equal(profile, "normal");
	zassert_equal(hook_calls, 0, "and the application was never told");
}

/* --- The application's verdict ------------------------------------------- */

ZTEST(tedge_parameters, test_the_application_can_refuse_a_change)
{
	char reason[128];
	int32_t interval;

	hook_verdict = -EBUSY;
	zassert_equal(apply("{\"interval_s\":60}", reason, sizeof(reason)),
		      -EBUSY);
	zassert_equal(hook_calls, 1, "the hook is called once");
	zassert_str_equal(reason, "the pump is running",
			  "and its reason is what the operation carries");
	zassert_ok(tedge_parameter_get_int("pump", "interval_s", &interval));
	zassert_equal(interval, 30, "the previous value is what runs");
	zassert_equal(store_writes, 0, "and a refusal costs no flash");
}

ZTEST(tedge_parameters, test_a_refused_change_leaves_strings_alone)
{
	char reason[128];
	char site[16];

	zassert_ok(apply("{\"site\":\"shed\"}", reason, sizeof(reason)));
	hook_verdict = -EBUSY;
	zassert_equal(apply("{\"site\":\"barn\"}", reason, sizeof(reason)),
		      -EBUSY);
	zassert_ok(tedge_parameter_get_string("pump", "site", site,
					      sizeof(site)));
	zassert_str_equal(site, "shed", "rolled back to the stored value");
}

/* --- Storage -------------------------------------------------------------- */

ZTEST(tedge_parameters, test_only_what_changed_is_stored)
{
	char reason[128];

	/* interval_s moves; auto_mode is set to the value it already has. */
	zassert_ok(apply("{\"interval_s\":60,\"auto_mode\":true}", reason,
			 sizeof(reason)));
	zassert_equal(store_writes, 1,
		      "one key written, not one per value in the change");
}

ZTEST(tedge_parameters, test_a_stored_value_uses_its_own_key)
{
	char reason[128];
	bool found = false;

	zassert_ok(apply("{\"interval_s\":60}", reason, sizeof(reason)));
	for (int i = 0; i < STORE_SLOTS; i++) {
		if (store_slot[i].used &&
		    strcmp(store_slot[i].key, "tedge/param/pump/interval_s") ==
			    0) {
			found = true;
		}
	}
	zassert_true(found, "one settings key per parameter");
}

/* --- The twin -------------------------------------------------------------*/

ZTEST(tedge_parameters, test_the_twin_is_the_whole_set)
{
	char reason[128];

	zassert_ok(apply("{\"interval_s\":60,\"profile\":\"boost\"}", reason,
			 sizeof(reason)));
	zassert_equal(twin_publishes, 1, "one message per accepted change");
	zassert_str_equal(twin_fragment, "pump",
			  "the fragment is named after the set");
	zassert_str_equal(twin_json,
			  "{\"interval_s\":60,\"auto_mode\":true,"
			  "\"profile\":\"boost\",\"site\":\"\"}",
			  "every declared value, typed, not just what moved");
}

ZTEST(tedge_parameters, test_a_refused_change_reports_nothing)
{
	char reason[128];

	hook_verdict = -EBUSY;
	(void)apply("{\"interval_s\":60}", reason, sizeof(reason));
	zassert_equal(twin_publishes, 0,
		      "what the cloud shows is what the device runs");
}

/* --- The schema ---------------------------------------------------------- */

ZTEST(tedge_parameters, test_the_schema_describes_the_declaration)
{
	char schema[1024];

	zassert_true(tedge_params_schema("pump", schema, sizeof(schema)) > 0);

	zassert_not_null(strstr(schema, "\"identifier\":\"pump\""),
			 "the identifier is the set's name: %s", schema);
	zassert_not_null(
		strstr(schema, "\"contexts\":[\"asset\",\"event\",\"operation\"]"),
		"asset and operation, or the UI will not let anyone edit it");
	zassert_not_null(strstr(schema, "\"type\":\"integer\""), "%s", schema);
	zassert_not_null(strstr(schema, "\"minimum\":5"), "%s", schema);
	zassert_not_null(strstr(schema, "\"maximum\":3600"), "%s", schema);
	zassert_not_null(strstr(schema, "\"type\":\"boolean\""), "%s", schema);
	zassert_not_null(strstr(schema, "\"maxLength\":8"), "%s", schema);
	zassert_not_null(
		strstr(schema, "\"enum\":[\"normal\",\"quiet\",\"boost\"]"),
		"%s", schema);
	zassert_not_null(strstr(schema, "\"order\":1"),
			 "the UI lays the fields out in the declared order");
	zassert_not_null(strstr(schema, "\"description\":\"Seconds between reads\""),
			 "%s", schema);
	/* A description longer than any fixed buffer comes out whole. */
	zassert_not_null(strstr(schema, "\"Operating profile. Quiet trades flow for noise at night; boost runs past the rated speed for a short while, and is logged as it does\""),
			 "a long description was cut: %s", schema);
}

ZTEST(tedge_parameters, test_the_schema_says_when_it_does_not_fit)
{
	char schema[64];

	zassert_equal(tedge_params_schema("pump", schema, sizeof(schema)),
		      -ENOMEM);
	zassert_str_equal(schema, "",
			  "rather than handing back half a schema");
}

ZTEST(tedge_parameters, test_no_schema_for_a_set_nobody_declared)
{
	char schema[256];

	zassert_equal(tedge_params_schema("boiler", schema, sizeof(schema)),
		      -ENOENT);
}

ZTEST_SUITE(tedge_parameters, NULL, params_setup, params_before, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* One-time passwords                                                        */
/* ------------------------------------------------------------------------ */

/* A password issued by a provisioning server is not the 32 characters the
 * device would generate itself; any length up to the maximum must pass. */
ZTEST(tedge_otp, test_supplied_lengths_accepted)
{
	char buf[TEDGE_OTP_MAX + 2];

	zassert_true(tedge_otp_valid("x"));
	zassert_true(tedge_otp_valid("fixture-token"));
	memset(buf, 'a', TEDGE_OTP_MAX);
	buf[TEDGE_OTP_MAX] = '\0';
	zassert_true(tedge_otp_valid(buf), "the maximum length must be accepted");
	buf[TEDGE_OTP_MAX] = 'a';
	buf[TEDGE_OTP_MAX + 1] = '\0';
	zassert_false(tedge_otp_valid(buf), "one past the maximum must not");
}

ZTEST(tedge_otp, test_unusable_passwords_rejected)
{
	zassert_false(tedge_otp_valid(NULL));
	zassert_false(tedge_otp_valid(""));
	zassert_false(tedge_otp_valid("has space"));
	zassert_false(tedge_otp_valid("tab\there"));
	zassert_false(tedge_otp_valid("new\nline"));
}

/* The longest external ID with the longest password must fit the buffer
 * the enrollment request builds, and anything longer must be an error, not
 * a silently shortened credential. */
ZTEST(tedge_otp, test_basic_credential_never_truncates)
{
	char id[64], pw[TEDGE_OTP_MAX + 1];
	char out[64 + 1 + TEDGE_OTP_MAX + 1]; /* as in tedge_enroll.c */

	memset(id, 'i', sizeof(id) - 1);
	id[sizeof(id) - 1] = '\0';
	memset(pw, 'p', sizeof(pw) - 1);
	pw[sizeof(pw) - 1] = '\0';

	zassert_equal(tedge_basic_credential(out, sizeof(out), id, pw),
		      63 + 1 + TEDGE_OTP_MAX);
	zassert_equal(out[63], ':');
	zassert_equal(tedge_basic_credential(out, 16, id, pw), -ENAMETOOLONG);
	zassert_equal(tedge_basic_credential(out, sizeof(out), "dev", "pw"), 6);
	zassert_str_equal(out, "dev:pw");
}

ZTEST_SUITE(tedge_otp, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Firmware versions                                                         */
/*                                                                           */
/* The bootloader's header holds only MAJOR.MINOR.PATCH, so a pre-release    */
/* installed by version must be recognised by the application's own string. */
/* ------------------------------------------------------------------------ */

ZTEST(tedge_fw, test_application_version_wins)
{
	char v[24];

	zassert_ok(tedge_fw_version_pick("0.4.0-rc1", "0.4.0", v, sizeof(v)));
	zassert_str_equal(v, "0.4.0-rc1");
}

ZTEST(tedge_fw, test_header_when_the_application_gives_none)
{
	char v[24];

	zassert_ok(tedge_fw_version_pick("", "0.4.0", v, sizeof(v)));
	zassert_str_equal(v, "0.4.0");
	zassert_ok(tedge_fw_version_pick(NULL, "0.4.0", v, sizeof(v)));
	zassert_str_equal(v, "0.4.0");
	zassert_equal(tedge_fw_version_pick("", "", v, sizeof(v)), -ENOENT);
	zassert_equal(tedge_fw_version_pick("0.4.0-rc1", "0.4.0", v, 4),
		      -ENOSPC);
}

ZTEST(tedge_fw, test_the_running_pre_release_is_refused)
{
	zassert_true(tedge_fw_is_running("app", "0.4.0-rc1", "app",
					 "0.4.0-rc1"));
}

ZTEST(tedge_fw, test_the_final_release_over_its_pre_release_is_accepted)
{
	zassert_false(tedge_fw_is_running("app", "0.4.0-rc1", "app", "0.4.0"));
	zassert_false(tedge_fw_is_running("app", "0.4.0", "app", "0.4.0-rc1"));
	/* Same version under another firmware name is another image. */
	zassert_false(tedge_fw_is_running("app", "0.4.0", "other", "0.4.0"));
	/* Nothing known about the running image: never refuse. */
	zassert_false(tedge_fw_is_running("app", "", "app", "0.4.0"));
}

ZTEST(tedge_fw, test_a_pre_release_is_installed_not_rolled_back)
{
	/* Test boot of 0.4.0-rc1: confirm it. */
	zassert_equal(tedge_fw_boot_outcome(false, "0.4.0-rc1", "0.4.0-rc1"),
		      TEDGE_FW_BOOT_TEST);
	/* Confirmed and it is the one that was installed. */
	zassert_equal(tedge_fw_boot_outcome(true, "0.4.0-rc1", "0.4.0-rc1"),
		      TEDGE_FW_BOOT_DONE);
	/* What a header-only version would have concluded. */
	zassert_equal(tedge_fw_boot_outcome(true, "0.4.0", "0.4.0-rc1"),
		      TEDGE_FW_BOOT_REVERTED);
}

ZTEST(tedge_fw, test_a_reverted_image_is_reported)
{
	zassert_equal(tedge_fw_boot_outcome(true, "0.3.1", "0.4.0"),
		      TEDGE_FW_BOOT_REVERTED);
}

ZTEST_SUITE(tedge_fw, NULL, NULL, NULL, NULL, NULL);
