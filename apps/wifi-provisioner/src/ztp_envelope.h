/* SPDX-License-Identifier: Apache-2.0
 *
 * The device's EnrollRequest and the SignedEnvelope around it
 * (lab-ztp-provisioner, pkg/protocol/wire.go and sign.go), p256 suite.
 */

#ifndef ZTP_ENVELOPE_H_
#define ZTP_ENVELOPE_H_

#include <stddef.h>
#include <stdint.h>

struct ztp_request_info {
	const char *device_id;     /* the device's ZTP identity, e.g. its hostname */
	const char *hostname;
	const char *model;         /* board name */
	const char *mac;           /* "aa:bb:cc:dd:ee:ff", or "" to omit */
	const char *agent_version;
	int64_t unix_s;            /* best-known wall-clock time */
	int max_response_bytes;
};

/**
 * @brief Render the canonical (RFC 8785) EnrollRequest JSON.
 *
 * Canonical by construction: the fields are printed in sorted order from a
 * single format string, not canonicalised afterwards. Every string must be
 * printable ASCII without '"' or '\\', so nothing needs escaping; anything
 * else is rejected rather than escaped differently from the Go encoder.
 *
 * @return length written, -EINVAL for a value that would need escaping,
 *         -ENOSPC when @p out is too small.
 */
int ztp_request_canonical(const struct ztp_request_info *in,
			  const char *nonce_b64, const char *pubkey_b64,
			  const char *ephemeral_b64, char *out, size_t out_size);

/**
 * @brief Start a new session and build the signed envelope for it.
 *
 * Creates a fresh ephemeral key (ztp_session_new) and nonce, signs the
 * canonical request with the identity key, and writes the SignedEnvelope
 * JSON the relay forwards to the server.
 *
 * @return length written, or a negative errno.
 */
int ztp_envelope_build(const struct ztp_request_info *in, char *out,
		       size_t out_size);

#endif /* ZTP_ENVELOPE_H_ */
