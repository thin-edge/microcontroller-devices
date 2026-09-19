/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the module's pure helpers: SmartREST CSV, the reconnect
 * back-off and the PKCS#7 unwrap that reads the enrollment reply.
 */

#include <zephyr/ztest.h>

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
