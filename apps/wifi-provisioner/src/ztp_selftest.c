/* SPDX-License-Identifier: Apache-2.0
 *
 * Boot-time self-test of the ZTP p256 crypto against vectors from the Go
 * implementation (ztp_selftest_vectors.h). It answers, on real hardware, the
 * two questions the ztp-ble-provisioner spike exists for: does PSA on this
 * board do every operation the suite needs, and does it agree with the
 * server byte for byte. It logs one PASS/FAIL line per check and the time
 * each took, then a summary.
 */

#include "ztp_crypto.h"
#include "ztp_selftest.h"
#include "ztp_selftest_vectors.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/base64.h>

LOG_MODULE_REGISTER(ztp_selftest, CONFIG_LOG_DEFAULT_LEVEL);

static int failures;

static void report(const char *what, int rc, int64_t start)
{
	int64_t ms = k_uptime_get() - start;

	if (rc) {
		failures++;
		LOG_ERR("FAIL %-34s (%d) %lld ms", what, rc, ms);
	} else {
		LOG_INF("PASS %-34s %lld ms", what, ms);
	}
}

/* The server's sealed c8y.v2 vector must open to exactly its plaintext. */
static int check_open_vector(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	uint8_t out[sizeof(sv_plaintext) + 8];
	psa_key_id_t key;
	size_t len;
	int rc;

	ztp_session_key_attributes(&attr);
	if (psa_import_key(&attr, sv_device_scalar, sizeof(sv_device_scalar),
			   &key) != PSA_SUCCESS) {
		psa_reset_key_attributes(&attr);
		return -EIO;
	}
	psa_reset_key_attributes(&attr);

	/* Importing the scalar must reproduce the vector's public point, or
	 * the two sides disagree on the key encoding before any ECDH. */
	uint8_t point[ZTP_P256_POINT_LEN];

	if (psa_export_public_key(key, point, sizeof(point), &len) !=
		    PSA_SUCCESS ||
	    len != sizeof(point) || memcmp(point, sv_device_point, len) != 0) {
		psa_destroy_key(key);
		return -EINVAL;
	}

	rc = ztp_open_with_key(key, sv_server_point, sv_nonce, sv_ciphertext,
			       sizeof(sv_ciphertext), out, sizeof(out), &len);
	if (rc == 0 &&
	    (len != sizeof(sv_plaintext) || memcmp(out, sv_plaintext, len))) {
		rc = -EINVAL;
	}

	/* One flipped ciphertext byte must fail the tag, not decrypt. */
	if (rc == 0) {
		uint8_t bad[sizeof(sv_ciphertext)];

		memcpy(bad, sv_ciphertext, sizeof(bad));
		bad[0] ^= 0xff;
		if (ztp_open_with_key(key, sv_server_point, sv_nonce, bad,
				      sizeof(bad), out, sizeof(out),
				      &len) != -EBADMSG) {
			rc = -EFAULT;
		}
	}
	psa_destroy_key(key);
	return rc;
}

/* The server's signature over a canonical EnrollRequest must verify, and must
 * stop verifying when one byte of the signed bytes changes. */
static int check_verify_vector(void)
{
	uint8_t canon[sizeof(gv_canon)];
	int rc = ztp_verify(gv_pubkey, gv_canon, sizeof(gv_canon), gv_sig);

	if (rc) {
		return rc;
	}
	memcpy(canon, gv_canon, sizeof(canon));
	canon[sizeof(canon) / 2] ^= 0x01;
	return ztp_verify(gv_pubkey, canon, sizeof(canon), gv_sig) == -EBADMSG
		       ? 0
		       : -EFAULT;
}

/* The persistent identity key signs, and its signature verifies against its
 * own exported point — the round trip the server performs on every request. */
static int check_identity(void)
{
	uint8_t pub[ZTP_P256_POINT_LEN];
	uint8_t sig[ZTP_P256_SIG_LEN];
	char b64[92];
	size_t olen;
	int rc;

	rc = ztp_identity_public(pub);
	if (rc) {
		return rc;
	}
	rc = ztp_identity_sign(gv_canon, sizeof(gv_canon), sig);
	if (rc) {
		return rc;
	}
	rc = ztp_verify(pub, gv_canon, sizeof(gv_canon), sig);
	if (rc == 0 && base64_encode(b64, sizeof(b64), &olen, pub,
				     sizeof(pub)) == 0) {
		LOG_INF("identity public_key %s", b64);
	}
	return rc;
}

static int check_session(void)
{
	uint8_t pub[ZTP_P256_POINT_LEN];
	int rc = ztp_session_new(pub);

	ztp_session_end();
	return rc;
}

int ztp_selftest_run(void)
{
	int64_t t;
	int rc;

	failures = 0;
	LOG_INF("ZTP p256 self-test starting");

	/* PSA ITS keeps the identity key in settings. */
	rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings init failed (%d)", rc);
		return rc;
	}
	rc = ztp_crypto_init();
	if (rc) {
		return rc;
	}

	t = k_uptime_get();
	report("open sealed c8y.v2 vector", check_open_vector(), t);
	t = k_uptime_get();
	report("verify server signature vector", check_verify_vector(), t);
	t = k_uptime_get();
	report("identity key sign + verify", check_identity(), t);
	t = k_uptime_get();
	report("new session key", check_session(), t);

	if (failures) {
		LOG_ERR("ZTP p256 self-test: %d FAILED", failures);
		return -EIO;
	}
	LOG_INF("ZTP p256 self-test: all PASSED");
	return 0;
}
