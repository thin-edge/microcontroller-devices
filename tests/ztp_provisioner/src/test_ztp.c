/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the ZTP provisioner's protocol code. Every expected value
 * comes from lab-ztp-provisioner's Go implementation — its canonicaliser, its
 * text renderer, its sealing — never from this code's own output, so a
 * divergence from the server fails here rather than as an enrollment that
 * silently does not work.
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/base64.h>

#include <errno.h>
#include <string.h>

#include "ztp_crypto.h"
#include "ztp_envelope.h"
#include "ztp_manifest.h"
#include "ztp_selftest_vectors.h"
#include "fixtures.h"

/* ------------------------------------------------------------------------ */
/* Canonical EnrollRequest (task 4.5)                                        */
/* ------------------------------------------------------------------------ */

static const struct ztp_request_info gv_info = {
	.device_id = "zephyr-aabbccddeeff",
	.hostname = "tedge-modbus",
	.model = "esp32c6_devkitc",
	.mac = "aa:bb:cc:dd:ee:ff",
	.agent_version = "dev",
	.unix_s = GV_UNIX_S,
	.max_response_bytes = 4096,
};

ZTEST(ztp_envelope, test_canonical_matches_go)
{
	static char out[768];
	int n = ztp_request_canonical(&gv_info, GV_NONCE, GV_PUBLIC_KEY,
				      GV_EPHEMERAL, out, sizeof(out));

	zassert_true(n > 0, "render failed (%d)", n);
	zassert_equal(n, sizeof(gv_canon), "length %d, Go produced %zu:\n%s", n,
		      sizeof(gv_canon), out);
	zassert_mem_equal(out, gv_canon, sizeof(gv_canon),
			  "differs from the Go canonicaliser:\n%s", out);
}

ZTEST(ztp_envelope, test_canonical_omits_empty_mac)
{
	static char out[768];
	struct ztp_request_info in = gv_info;

	in.mac = "";
	zassert_true(ztp_request_canonical(&in, GV_NONCE, GV_PUBLIC_KEY,
					   GV_EPHEMERAL, out, sizeof(out)) > 0);
	/* omitempty: absent, not [] */
	zassert_is_null(strstr(out, "mac_addresses"), "%s", out);
	zassert_not_null(strstr(out, "\"hostname\":\"tedge-modbus\",\"model\""),
			 "%s", out);
}

ZTEST(ztp_envelope, test_canonical_rejects_values_needing_escapes)
{
	static char out[768];
	struct ztp_request_info in = gv_info;

	in.device_id = "evil\"id";
	zassert_equal(ztp_request_canonical(&in, GV_NONCE, GV_PUBLIC_KEY,
					    GV_EPHEMERAL, out, sizeof(out)),
		      -EINVAL);
	in.device_id = "back\\slash";
	zassert_equal(ztp_request_canonical(&in, GV_NONCE, GV_PUBLIC_KEY,
					    GV_EPHEMERAL, out, sizeof(out)),
		      -EINVAL);
}

ZTEST(ztp_envelope, test_canonical_too_small)
{
	char out[64];

	zassert_equal(ztp_request_canonical(&gv_info, GV_NONCE, GV_PUBLIC_KEY,
					    GV_EPHEMERAL, out, sizeof(out)),
		      -ENOSPC);
}

ZTEST_SUITE(ztp_envelope, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Text response and manifest (task 4.7)                                     */
/* ------------------------------------------------------------------------ */

ZTEST(ztp_manifest, test_rfc3339)
{
	int64_t t;

	zassert_ok(ztp_parse_rfc3339("2026-01-01T12:00:00Z", 20, &t));
	zassert_equal(t, GV_UNIX_S);
	zassert_ok(ztp_parse_rfc3339("1970-01-01T00:00:00Z", 20, &t));
	zassert_equal(t, 0);
	/* Go's RFC3339 time sync carries no fraction, but RFC3339Nano does. */
	zassert_ok(ztp_parse_rfc3339("2026-01-01T12:00:00.123456789Z", 30, &t));
	zassert_equal(t, GV_UNIX_S);
	zassert_ok(ztp_parse_rfc3339("2024-02-29T00:00:00Z", 20, &t));
	zassert_equal(t, 1709164800LL);
	zassert_equal(ztp_parse_rfc3339("2026-01-01T12:00:00+02:00", 25, &t),
		      -EINVAL, "offsets are not produced and must not parse");
	zassert_equal(ztp_parse_rfc3339("2026-13-01T12:00:00Z", 20, &t), -EINVAL);
	zassert_equal(ztp_parse_rfc3339("garbage", 7, &t), -EINVAL);
}

ZTEST(ztp_manifest, test_rfc3339_format_round_trip)
{
	static const int64_t cases[] = { 0, GV_UNIX_S, 1709164800LL /* leap day */,
					 951782400LL /* 2000-02-29 */,
					 4102444799LL /* 2099-12-31T23:59:59 */ };
	char s[21];
	int64_t back;

	ztp_format_rfc3339(GV_UNIX_S, s);
	zassert_str_equal(s, "2026-01-01T12:00:00Z");
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		ztp_format_rfc3339(cases[i], s);
		zassert_ok(ztp_parse_rfc3339(s, strlen(s), &back), "%s", s);
		zassert_equal(back, cases[i], "%s", s);
	}
}

ZTEST(ztp_manifest, test_accepted_response)
{
	struct ztp_response r;

	zassert_ok(ztp_response_parse(fx_accepted, strlen(fx_accepted), &r));
	zassert_equal(r.status, ZTP_STATUS_ACCEPTED);
	zassert_true(r.manifest.len > 0);
	zassert_false(r.encrypted);
	zassert_true(r.server_time > 0, "server_time record not read");
}

ZTEST(ztp_manifest, test_pending_response)
{
	struct ztp_response r;

	zassert_ok(ztp_response_parse(fx_pending, strlen(fx_pending), &r));
	zassert_equal(r.status, ZTP_STATUS_PENDING);
	zassert_equal(r.retry_after, 30);
	zassert_true(ztp_slice_eq(r.reason, "awaiting operator approval"));
	zassert_equal(r.manifest.len, 0);
	zassert_true(r.server_time > 0);
}

ZTEST(ztp_manifest, test_response_needs_version_and_status)
{
	struct ztp_response r;
	const char *no_version = "status=accepted\n";
	const char *wrong_version = "protocol_version=2\nstatus=accepted\n";
	const char *no_status = "protocol_version=1\n";

	zassert_equal(ztp_response_parse(no_version, strlen(no_version), &r),
		      -EPROTO);
	zassert_equal(ztp_response_parse(wrong_version, strlen(wrong_version), &r),
		      -EPROTO);
	zassert_equal(ztp_response_parse(no_status, strlen(no_status), &r),
		      -EPROTO);
}

ZTEST(ztp_manifest, test_response_ignores_unknown_keys)
{
	struct ztp_response r;
	const char *s = "protocol_version=1\nfuture.thing=x\nstatus=rejected\r\n"
			"not a record\nreason=nope\n";

	zassert_ok(ztp_response_parse(s, strlen(s), &r));
	zassert_equal(r.status, ZTP_STATUS_REJECTED);
	zassert_true(ztp_slice_eq(r.reason, "nope"), "CRLF must not leak in");
}

/* Decode the fixture's manifest.payload into @p buf. */
static size_t fixture_manifest(char *buf, size_t size)
{
	struct ztp_response r;
	size_t olen;

	zassert_ok(ztp_response_parse(fx_accepted, strlen(fx_accepted), &r));
	zassert_ok(base64_decode((uint8_t *)buf, size - 1, &olen,
				 (const uint8_t *)r.manifest.p, r.manifest.len));
	buf[olen] = '\0';
	return olen;
}

ZTEST(ztp_manifest, test_manifest_modules)
{
	static char man[2048];
	size_t len = fixture_manifest(man, sizeof(man));
	const char *cur = man;
	struct ztp_manifest_hdr hdr;
	struct ztp_module m;
	bool wifi = false, c8y = false, ssh = false;
	int rc;

	ztp_manifest_header(man, len, &hdr);
	zassert_true(hdr.version_ok);
	zassert_true(ztp_slice_eq(hdr.device_id, "tedge-modbus"));

	while ((rc = ztp_manifest_next_module(&cur, man + len, &m)) == 1) {
		if (ztp_slice_eq(m.type, "wifi.v2")) {
			wifi = true;
			zassert_false(m.sealed);
			zassert_true(m.payload.len > 0);
		} else if (ztp_slice_eq(m.type, "c8y.v2")) {
			c8y = true;
			zassert_true(m.sealed, "the token must arrive sealed");
			zassert_true(ztp_slice_eq(m.format, "raw"));
		} else if (ztp_slice_eq(m.type, "ssh.authorized_keys.v2")) {
			ssh = true;
		}
	}
	zassert_equal(rc, 0);
	zassert_true(wifi && c8y && ssh, "wifi %d c8y %d ssh %d", wifi, c8y, ssh);
}

ZTEST(ztp_manifest, test_malformed_sealed_line)
{
	const char *s = "module-sealed=c8y.v2 raw onlytwo\n";
	const char *cur = s;
	struct ztp_module m;

	zassert_equal(ztp_manifest_next_module(&cur, s + strlen(s), &m), -EPROTO);
}

ZTEST_SUITE(ztp_manifest, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* p256 crypto against the Go vectors                                        */
/* ------------------------------------------------------------------------ */

static psa_key_id_t import_scalar(const uint8_t *scalar)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key;

	ztp_session_key_attributes(&attr);
	zassert_equal(psa_import_key(&attr, scalar, 32, &key), PSA_SUCCESS);
	psa_reset_key_attributes(&attr);
	return key;
}

ZTEST(ztp_crypto, test_open_seal_vector)
{
	uint8_t out[sizeof(sv_plaintext) + 8];
	psa_key_id_t key = import_scalar(sv_device_scalar);
	size_t len;

	zassert_ok(ztp_open_with_key(key, sv_server_point, sv_nonce,
				     sv_ciphertext, sizeof(sv_ciphertext), out,
				     sizeof(out), &len));
	zassert_equal(len, sizeof(sv_plaintext));
	zassert_mem_equal(out, sv_plaintext, len);
	psa_destroy_key(key);
}

ZTEST(ztp_crypto, test_tampered_seal_fails)
{
	uint8_t out[sizeof(sv_plaintext) + 8];
	uint8_t bad[sizeof(sv_ciphertext)];
	psa_key_id_t key = import_scalar(sv_device_scalar);
	size_t len;

	memcpy(bad, sv_ciphertext, sizeof(bad));
	bad[sizeof(bad) - 1] ^= 0x01; /* the tag itself */
	zassert_equal(ztp_open_with_key(key, sv_server_point, sv_nonce, bad,
					sizeof(bad), out, sizeof(out), &len),
		      -EBADMSG);
	psa_destroy_key(key);
}

ZTEST(ztp_crypto, test_verify_signature_vector)
{
	uint8_t canon[sizeof(gv_canon)];

	zassert_ok(ztp_verify(gv_pubkey, gv_canon, sizeof(gv_canon), gv_sig));
	memcpy(canon, gv_canon, sizeof(canon));
	canon[0] ^= 0x01;
	zassert_equal(ztp_verify(gv_pubkey, canon, sizeof(canon), gv_sig),
		      -EBADMSG);
}

/* The whole receive path on a real server rendering: parse the response,
 * decode the manifest, open the sealed c8y.v2 module with the device key it
 * was sealed to, and find the fields the provisioner stores. */
ZTEST(ztp_crypto, test_fixture_sealed_c8y_opens)
{
	static char man[2048];
	size_t len = fixture_manifest(man, sizeof(man));
	const char *cur = man;
	struct ztp_module m;
	uint8_t eph[ZTP_P256_POINT_LEN], nonce[ZTP_SEAL_NONCE_LEN];
	static uint8_t ct[512], pt[512];
	size_t olen, ptlen;
	psa_key_id_t key;

	do {
		zassert_equal(ztp_manifest_next_module(&cur, man + len, &m), 1,
			      "no sealed c8y.v2 module");
	} while (!ztp_slice_eq(m.type, "c8y.v2"));

	zassert_ok(base64_decode(eph, sizeof(eph), &olen,
				 (const uint8_t *)m.eph.p, m.eph.len));
	zassert_ok(base64_decode(nonce, sizeof(nonce), &olen,
				 (const uint8_t *)m.nonce.p, m.nonce.len));
	zassert_ok(base64_decode(ct, sizeof(ct), &olen,
				 (const uint8_t *)m.ciphertext.p,
				 m.ciphertext.len));

	key = import_scalar(fx_device_scalar);
	zassert_ok(ztp_open_with_key(key, eph, nonce, ct, olen, pt,
				     sizeof(pt) - 1, &ptlen));
	psa_destroy_key(key);
	pt[ptlen] = '\0';
	zassert_not_null(strstr((char *)pt, "url=https://example.cumulocity.com"));
	zassert_not_null(strstr((char *)pt, "external_id=zephyr-tedge-modbus"));
	zassert_not_null(strstr((char *)pt, "one_time_password=fixture-token"));
}

ZTEST(ztp_crypto, test_session_key_round_trip)
{
	uint8_t pub[ZTP_P256_POINT_LEN];

	zassert_ok(ztp_session_new(pub));
	zassert_equal(pub[0], 0x04, "not an uncompressed point");
	ztp_session_end();
	zassert_equal(ztp_open(sv_server_point, sv_nonce, sv_ciphertext,
			       sizeof(sv_ciphertext), pub, sizeof(pub), NULL),
		      -ENOENT, "no session after ztp_session_end()");
}

static void *crypto_setup(void)
{
	zassert_ok(ztp_crypto_init());
	return NULL;
}

ZTEST_SUITE(ztp_crypto, NULL, crypto_setup, NULL, NULL, NULL);
