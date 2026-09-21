/* SPDX-License-Identifier: Apache-2.0
 *
 * Parsers for the text renderings of lab-ztp-provisioner: the enroll response
 * (pkg/protocol/enrolltext.go) and the bundle manifest inside it
 * (pkg/protocol/textmanifest.go). Both are `key=value` lines, so one
 * iterator serves both; nothing here needs a JSON parser.
 *
 * Parsing is non-destructive: results point into the caller's buffer.
 * Unknown keys are ignored at both levels, as the format requires.
 */

#ifndef ZTP_MANIFEST_H_
#define ZTP_MANIFEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ztp_slice {
	const char *p;
	size_t len;
};

/** @brief One `key=value` line. */
struct ztp_kv {
	struct ztp_slice key;
	struct ztp_slice val;
};

/**
 * @brief Advance to the next `key=value` line in [*cur, end).
 *
 * @return true with @p kv filled, false at the end of the input. Lines
 *         without '=' are skipped.
 */
bool ztp_kv_next(const char **cur, const char *end, struct ztp_kv *kv);

/** @brief True when @p s equals the NUL-terminated @p lit. */
bool ztp_slice_eq(struct ztp_slice s, const char *lit);

/**
 * @brief Parse an RFC 3339 UTC time "YYYY-MM-DDTHH:MM:SS[.frac]Z".
 *
 * @return 0 with @p unix_s set, -EINVAL otherwise.
 */
int ztp_parse_rfc3339(const char *s, size_t len, int64_t *unix_s);

/**
 * @brief Format unix seconds as "YYYY-MM-DDTHH:MM:SSZ", the form Go's
 * time.Time marshals whole seconds to.
 *
 * @param out at least 21 bytes
 */
void ztp_format_rfc3339(int64_t unix_s, char out[21]);

enum ztp_status {
	ZTP_STATUS_UNKNOWN,
	ZTP_STATUS_ACCEPTED,
	ZTP_STATUS_PENDING,
	ZTP_STATUS_REJECTED,
};

/** @brief The records of a text enroll response that the device uses. */
struct ztp_response {
	enum ztp_status status;
	struct ztp_slice reason;
	int retry_after;             /* seconds, 0 when absent */
	int64_t server_time;         /* unix seconds, 0 when absent */
	struct ztp_slice manifest;   /* base64 manifest.payload */
	bool encrypted;              /* encrypted.* present (not requested) */
};

/**
 * @brief Parse a text enroll response.
 *
 * @return 0; -EPROTO when protocol_version is not "1" or status is missing.
 */
int ztp_response_parse(const char *buf, size_t len, struct ztp_response *r);

/** @brief One module of a manifest, clear or sealed. */
struct ztp_module {
	struct ztp_slice type;
	bool sealed;
	struct ztp_slice payload;    /* clear: base64 payload */
	struct ztp_slice format;     /* sealed: "raw" or "json" */
	struct ztp_slice eph;        /* sealed: base64 server ephemeral point */
	struct ztp_slice nonce;      /* sealed: base64 nonce */
	struct ztp_slice ciphertext; /* sealed: base64 ciphertext + tag */
};

/** @brief Header records of a decoded manifest. */
struct ztp_manifest_hdr {
	struct ztp_slice device_id;
	bool version_ok;
};

/**
 * @brief Read the header records of a decoded manifest (one full pass).
 */
void ztp_manifest_header(const char *buf, size_t len,
			 struct ztp_manifest_hdr *hdr);

/**
 * @brief Advance to the next module line of a decoded manifest.
 *
 * @return 1 with @p m filled, 0 at the end, -EPROTO for a malformed module
 *         line (which fails the whole bundle rather than being skipped: a
 *         module the server meant to send must not silently disappear).
 */
int ztp_manifest_next_module(const char **cur, const char *end,
			     struct ztp_module *m);

#endif /* ZTP_MANIFEST_H_ */
