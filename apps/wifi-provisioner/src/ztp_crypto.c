/* SPDX-License-Identifier: Apache-2.0
 *
 * ZTP p256 suite over PSA. See ztp_crypto.h.
 */

#include "ztp_crypto.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ztp_crypto, CONFIG_LOG_DEFAULT_LEVEL);

#define IDENTITY_KEY_ID ((psa_key_id_t)CONFIG_APP_PROV_ZTP_PSA_KEY_ID)
#define SIGN_ALG        PSA_ALG_ECDSA(PSA_ALG_SHA_256)
#define P256_PAIR       PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
#define P256_PUBLIC     PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1)

/* ECDH, then HKDF-SHA256 over the shared secret. A P-256 shared secret is a
 * field element, not a uniform string, so it is never used as a key directly.
 * The server side is deriveP256Key() in pkg/protocol/encrypt.go. */
#define SEAL_KDF_ALG PSA_ALG_KEY_AGREEMENT(PSA_ALG_ECDH, PSA_ALG_HKDF(PSA_ALG_SHA_256))
#define SEAL_AEAD_ALG PSA_ALG_CHACHA20_POLY1305

/* HKDF info, bound to the protocol and version. The salt is empty (HKDF then
 * uses HashLen zero bytes, as Go's hkdf.New does for a nil salt). Changing
 * this breaks interoperability; the server bumps the algorithm name instead. */
static const uint8_t seal_info[] = "ztp/seal/v1";

static psa_key_id_t session_key = PSA_KEY_ID_NULL;

int ztp_crypto_init(void)
{
	psa_status_t st = psa_crypto_init();

	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d)", (int)st);
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Identity                                                                  */
/* ------------------------------------------------------------------------ */

static int ensure_identity(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	psa_status_t st;

	st = psa_get_key_attributes(IDENTITY_KEY_ID, &attr);
	psa_reset_key_attributes(&attr);
	if (st == PSA_SUCCESS) {
		return 0;
	}

	psa_set_key_id(&attr, IDENTITY_KEY_ID);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_type(&attr, P256_PAIR);
	psa_set_key_bits(&attr, 256);
	/* No PSA_KEY_USAGE_EXPORT: the public half can always be exported, the
	 * private half never needs to leave PSA. */
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, SIGN_ALG);
	st = psa_generate_key(&attr, &id);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		LOG_ERR("generating the ZTP identity key failed (%d)", (int)st);
		return -EIO;
	}
	LOG_INF("ZTP identity key generated (P-256, PSA key 0x%08x)",
		(unsigned int)IDENTITY_KEY_ID);
	return 0;
}

int ztp_identity_public(uint8_t out[ZTP_P256_POINT_LEN])
{
	size_t len;
	int rc = ensure_identity();

	if (rc) {
		return rc;
	}
	if (psa_export_public_key(IDENTITY_KEY_ID, out, ZTP_P256_POINT_LEN,
				  &len) != PSA_SUCCESS ||
	    len != ZTP_P256_POINT_LEN) {
		return -EIO;
	}
	return 0;
}

int ztp_identity_sign(const uint8_t *msg, size_t len,
		      uint8_t sig[ZTP_P256_SIG_LEN])
{
	size_t sig_len;
	psa_status_t st;
	int rc = ensure_identity();

	if (rc) {
		return rc;
	}
	st = psa_sign_message(IDENTITY_KEY_ID, SIGN_ALG, msg, len, sig,
			      ZTP_P256_SIG_LEN, &sig_len);
	if (st != PSA_SUCCESS || sig_len != ZTP_P256_SIG_LEN) {
		LOG_ERR("signing failed (%d)", (int)st);
		return -EIO;
	}
	return 0;
}

int ztp_verify(const uint8_t pub[ZTP_P256_POINT_LEN], const uint8_t *msg,
	       size_t len, const uint8_t sig[ZTP_P256_SIG_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key;
	psa_status_t st;

	psa_set_key_type(&attr, P256_PUBLIC);
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_algorithm(&attr, SIGN_ALG);
	st = psa_import_key(&attr, pub, ZTP_P256_POINT_LEN, &key);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		return -EINVAL;
	}
	st = psa_verify_message(key, SIGN_ALG, msg, len, sig, ZTP_P256_SIG_LEN);
	psa_destroy_key(key);
	if (st == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG;
	}
	return st == PSA_SUCCESS ? 0 : -EIO;
}

/* ------------------------------------------------------------------------ */
/* Session key and sealed payloads                                           */
/* ------------------------------------------------------------------------ */

void ztp_session_key_attributes(psa_key_attributes_t *attr)
{
	psa_set_key_type(attr, P256_PAIR);
	psa_set_key_bits(attr, 256);
	psa_set_key_usage_flags(attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(attr, SEAL_KDF_ALG);
}

int ztp_session_new(uint8_t pub[ZTP_P256_POINT_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	size_t len;
	psa_status_t st;

	ztp_session_end();
	ztp_session_key_attributes(&attr);
	st = psa_generate_key(&attr, &session_key);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		session_key = PSA_KEY_ID_NULL;
		LOG_ERR("generating the session key failed (%d)", (int)st);
		return -EIO;
	}
	if (psa_export_public_key(session_key, pub, ZTP_P256_POINT_LEN, &len) !=
		    PSA_SUCCESS ||
	    len != ZTP_P256_POINT_LEN) {
		ztp_session_end();
		return -EIO;
	}
	return 0;
}

void ztp_session_end(void)
{
	if (session_key != PSA_KEY_ID_NULL) {
		psa_destroy_key(session_key);
		session_key = PSA_KEY_ID_NULL;
	}
}

/* Derive the one-shot ChaCha20-Poly1305 key for a payload. */
static int derive_aead_key(psa_key_id_t key,
			   const uint8_t peer[ZTP_P256_POINT_LEN],
			   psa_key_id_t *aead)
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t st;

	st = psa_key_derivation_setup(&op, SEAL_KDF_ALG);
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_key_agreement(
			&op, PSA_KEY_DERIVATION_INPUT_SECRET, key, peer,
			ZTP_P256_POINT_LEN);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_bytes(
			&op, PSA_KEY_DERIVATION_INPUT_INFO, seal_info,
			sizeof(seal_info) - 1);
	}
	if (st == PSA_SUCCESS) {
		psa_set_key_type(&attr, PSA_KEY_TYPE_CHACHA20);
		psa_set_key_bits(&attr, 256);
		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
		psa_set_key_algorithm(&attr, SEAL_AEAD_ALG);
		st = psa_key_derivation_output_key(&attr, &op, aead);
		psa_reset_key_attributes(&attr);
	}
	psa_key_derivation_abort(&op);
	if (st != PSA_SUCCESS) {
		LOG_ERR("seal key derivation failed (%d)", (int)st);
		return -EIO;
	}
	return 0;
}

int ztp_open_with_key(psa_key_id_t key, const uint8_t peer[ZTP_P256_POINT_LEN],
		      const uint8_t nonce[ZTP_SEAL_NONCE_LEN], const uint8_t *ct,
		      size_t ct_len, uint8_t *out, size_t out_size,
		      size_t *out_len)
{
	psa_key_id_t aead;
	psa_status_t st;
	int rc;

	if (ct_len < ZTP_SEAL_TAG_LEN) {
		return -EBADMSG;
	}
	rc = derive_aead_key(key, peer, &aead);
	if (rc) {
		return rc;
	}
	st = psa_aead_decrypt(aead, SEAL_AEAD_ALG, nonce, ZTP_SEAL_NONCE_LEN,
			      NULL, 0, ct, ct_len, out, out_size, out_len);
	psa_destroy_key(aead);
	if (st == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG; /* the AEAD tag did not verify */
	}
	return st == PSA_SUCCESS ? 0 : -EIO;
}

int ztp_open(const uint8_t peer[ZTP_P256_POINT_LEN],
	     const uint8_t nonce[ZTP_SEAL_NONCE_LEN], const uint8_t *ct,
	     size_t ct_len, uint8_t *out, size_t out_size, size_t *out_len)
{
	if (session_key == PSA_KEY_ID_NULL) {
		return -ENOENT;
	}
	return ztp_open_with_key(session_key, peer, nonce, ct, ct_len, out,
				 out_size, out_len);
}
