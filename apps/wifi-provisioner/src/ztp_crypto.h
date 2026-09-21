/* SPDX-License-Identifier: Apache-2.0
 *
 * Crypto for the ZTP provisioning protocol, p256 suite (lab-ztp-provisioner,
 * pkg/protocol/suite.go): ECDSA P-256 with SHA-256 for signatures, and
 * P-256 ECDH + HKDF-SHA256 + ChaCha20-Poly1305 for sealed payloads.
 *
 * Every operation goes through PSA. Wire encodings are PSA's own, which is
 * why the suite was defined around them: public keys are uncompressed SEC 1
 * points (psa_export_public_key) and signatures are raw r||s
 * (psa_sign_message), so nothing here converts to or from ASN.1.
 */

#ifndef ZTP_CRYPTO_H_
#define ZTP_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#define ZTP_P256_POINT_LEN 65 /* 0x04 || X || Y */
#define ZTP_P256_SIG_LEN   64 /* r || s */
#define ZTP_SEAL_NONCE_LEN 12
#define ZTP_SEAL_TAG_LEN   16

/** @brief Initialise PSA. Safe to call more than once. */
int ztp_crypto_init(void);

/**
 * @brief Export the device's ZTP identity public key, generating the key on
 * first use.
 *
 * The key is a persistent P-256 key in PSA ITS at
 * CONFIG_APP_PROV_ZTP_PSA_KEY_ID. It is never exportable: the server
 * identifies the device by this key, and a copy of the private half anywhere
 * else would let that copy enrol as the device.
 */
int ztp_identity_public(uint8_t out[ZTP_P256_POINT_LEN]);

/** @brief Sign @p msg with the identity key (ECDSA P-256, SHA-256, raw r||s). */
int ztp_identity_sign(const uint8_t *msg, size_t len,
		      uint8_t sig[ZTP_P256_SIG_LEN]);

/**
 * @brief Verify a raw r||s ECDSA P-256 / SHA-256 signature.
 *
 * Not needed while the device trusts bundles on first use; it is here for the
 * self-test and for pinning a server key later.
 */
int ztp_verify(const uint8_t pub[ZTP_P256_POINT_LEN], const uint8_t *msg,
	       size_t len, const uint8_t sig[ZTP_P256_SIG_LEN]);

/**
 * @brief Start a provisioning session: create a fresh ephemeral P-256 key
 * pair and export its public point for EnrollRequest.ephemeral_p256.
 *
 * The key is volatile, so a reboot forgets it, and it replaces any key from
 * an earlier session. A bundle sealed to an earlier session cannot be opened.
 */
int ztp_session_new(uint8_t pub[ZTP_P256_POINT_LEN]);

/** @brief Destroy the session key. */
void ztp_session_end(void);

/**
 * @brief Open a p256-hkdf-sha256-chacha20poly1305 payload addressed to the
 * current session key.
 *
 * @param peer   the server's ephemeral point from the sealed payload
 * @param out    receives the plaintext, @p ct_len - ZTP_SEAL_TAG_LEN bytes
 * @return 0, -ENOENT with no session, -EBADMSG when the tag does not verify
 *         (tampering, or a payload sealed to another key), -EIO otherwise.
 */
int ztp_open(const uint8_t peer[ZTP_P256_POINT_LEN],
	     const uint8_t nonce[ZTP_SEAL_NONCE_LEN], const uint8_t *ct,
	     size_t ct_len, uint8_t *out, size_t out_size, size_t *out_len);

/** @brief ztp_open() with an explicit key, for the self-test. */
int ztp_open_with_key(psa_key_id_t key, const uint8_t peer[ZTP_P256_POINT_LEN],
		      const uint8_t nonce[ZTP_SEAL_NONCE_LEN], const uint8_t *ct,
		      size_t ct_len, uint8_t *out, size_t out_size,
		      size_t *out_len);

/** @brief Attributes a key must have to be used with ztp_open_with_key(). */
void ztp_session_key_attributes(psa_key_attributes_t *attr);

#endif /* ZTP_CRYPTO_H_ */
