/* SPDX-License-Identifier: Apache-2.0 */

#include "ztp_envelope.h"
#include "ztp_crypto.h"
#include "ztp_manifest.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ztp_envelope, CONFIG_LOG_DEFAULT_LEVEL);

/* base64 of an n-byte input, plus the terminator base64_encode writes. */
#define B64_LEN(n) ((((n) + 2) / 3) * 4 + 1)

#define CANON_MAX 768

/* Printable ASCII other than '"' and '\\' needs no JSON escaping, so it
 * serialises identically here and in Go's encoder. */
static bool json_plain(const char *s)
{
	for (; *s; s++) {
		if (*s < 0x20 || *s > 0x7e || *s == '"' || *s == '\\') {
			return false;
		}
	}
	return true;
}

int ztp_request_canonical(const struct ztp_request_info *in,
			  const char *nonce_b64, const char *pubkey_b64,
			  const char *ephemeral_b64, char *out, size_t out_size)
{
	const char *strs[] = { in->device_id, in->hostname, in->model,
			       in->mac, in->agent_version };
	char ts[21];
	char mac_field[48] = "";
	int n;

	for (size_t i = 0; i < ARRAY_SIZE(strs); i++) {
		if (strs[i] == NULL || !json_plain(strs[i])) {
			return -EINVAL;
		}
	}
	/* Go's time.Time marshals whole seconds as RFC 3339 with no fraction. */
	ztp_format_rfc3339(in->unix_s, ts);
	/* mac_addresses is omitempty: leave it out rather than send []. */
	if (in->mac[0] != '\0') {
		snprintf(mac_field, sizeof(mac_field), ",\"mac_addresses\":[\"%s\"]",
			 in->mac);
	}

	/*
	 * CANONICAL ORDER. RFC 8785 sorts object members by key, and the server
	 * verifies the signature over exactly these bytes. Keep every key below
	 * — including the nested "facts" object — in lexicographic order, and
	 * add a field only where it sorts. The native_sim test compares this
	 * output against the Go canonicaliser (testdata/vectors/sign_p256.json).
	 */
	n = snprintf(out, out_size,
		     "{\"capabilities\":[\"wifi.v2\",\"c8y.v2\"],"
		     "\"device_id\":\"%s\","
		     "\"ephemeral_p256\":\"%s\","
		     "\"facts\":{"
		     "\"agent_version\":\"%s\","
		     "\"hostname\":\"%s\""
		     "%s,"
		     "\"model\":\"%s\","
		     "\"os\":\"zephyr\"},"
		     "\"max_response_bytes\":%d,"
		     "\"nonce\":\"%s\","
		     "\"protocol_version\":\"1\","
		     "\"public_key\":\"%s\","
		     "\"response_format\":\"text\","
		     "\"timestamp\":\"%s\"}",
		     in->device_id, ephemeral_b64, in->agent_version,
		     in->hostname, mac_field, in->model, in->max_response_bytes,
		     nonce_b64, pubkey_b64, ts);
	if (n < 0 || (size_t)n >= out_size) {
		return -ENOSPC;
	}
	return n;
}

int ztp_envelope_build(const struct ztp_request_info *in, char *out,
		       size_t out_size)
{
	uint8_t pub[ZTP_P256_POINT_LEN];
	uint8_t eph[ZTP_P256_POINT_LEN];
	uint8_t nonce[16];
	uint8_t sig[ZTP_P256_SIG_LEN];
	char pub_b64[B64_LEN(ZTP_P256_POINT_LEN)];
	char eph_b64[B64_LEN(ZTP_P256_POINT_LEN)];
	char nonce_b64[B64_LEN(sizeof(nonce))];
	char sig_b64[B64_LEN(ZTP_P256_SIG_LEN)];
	static __noinit char canon[CANON_MAX];
	static __noinit char canon_b64[B64_LEN(CANON_MAX)];
	size_t olen;
	int canon_len;
	int rc;
	int n;

	rc = ztp_identity_public(pub);
	if (rc == 0) {
		rc = ztp_session_new(eph);
	}
	if (rc == 0 && psa_generate_random(nonce, sizeof(nonce)) != PSA_SUCCESS) {
		rc = -EIO;
	}
	if (rc) {
		return rc;
	}
	(void)base64_encode(pub_b64, sizeof(pub_b64), &olen, pub, sizeof(pub));
	(void)base64_encode(eph_b64, sizeof(eph_b64), &olen, eph, sizeof(eph));
	(void)base64_encode(nonce_b64, sizeof(nonce_b64), &olen, nonce,
			    sizeof(nonce));

	canon_len = ztp_request_canonical(in, nonce_b64, pub_b64, eph_b64, canon,
					  sizeof(canon));
	if (canon_len < 0) {
		LOG_ERR("could not render the enrollment request (%d)", canon_len);
		return canon_len;
	}
	rc = ztp_identity_sign((const uint8_t *)canon, canon_len, sig);
	if (rc) {
		return rc;
	}
	(void)base64_encode(sig_b64, sizeof(sig_b64), &olen, sig, sizeof(sig));
	if (base64_encode(canon_b64, sizeof(canon_b64), &olen,
			  (const uint8_t *)canon, canon_len) != 0) {
		return -ENOSPC;
	}

	/* The envelope itself is not a signing input, so its field order only
	 * has to be valid JSON; it matches SignedEnvelope for readability. */
	n = snprintf(out, out_size,
		     "{\"protocol_version\":\"1\",\"key_id\":\"device\","
		     "\"alg\":\"ecdsa-p256-sha256\",\"payload\":\"%s\","
		     "\"signature\":\"%s\"}",
		     canon_b64, sig_b64);
	if (n < 0 || (size_t)n >= out_size) {
		return -ENOSPC;
	}
	return n;
}
