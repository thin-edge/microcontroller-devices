/* SPDX-License-Identifier: Apache-2.0 */

#include "ztp_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool ztp_slice_eq(struct ztp_slice s, const char *lit)
{
	size_t n = strlen(lit);

	return s.len == n && memcmp(s.p, lit, n) == 0;
}

bool ztp_kv_next(const char **cur, const char *end, struct ztp_kv *kv)
{
	while (*cur < end) {
		const char *line = *cur;
		const char *nl = memchr(line, '\n', end - line);
		const char *stop = nl ? nl : end;
		const char *eq;

		*cur = nl ? nl + 1 : end;
		if (stop > line && stop[-1] == '\r') {
			stop--;
		}
		eq = memchr(line, '=', stop - line);
		if (eq == NULL) {
			continue;
		}
		kv->key = (struct ztp_slice){ line, eq - line };
		kv->val = (struct ztp_slice){ eq + 1, stop - eq - 1 };
		return true;
	}
	return false;
}

/* Split off the next space-separated field of @p s. */
static bool next_field(struct ztp_slice *s, struct ztp_slice *field)
{
	while (s->len && s->p[0] == ' ') {
		s->p++;
		s->len--;
	}
	if (s->len == 0) {
		return false;
	}
	const char *sp = memchr(s->p, ' ', s->len);
	size_t n = sp ? (size_t)(sp - s->p) : s->len;

	*field = (struct ztp_slice){ s->p, n };
	s->p += n;
	s->len -= n;
	return true;
}

static int parse_uint(const char *s, size_t n, int *out)
{
	int v = 0;

	if (n == 0 || n > 9) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return -EINVAL;
		}
		v = v * 10 + (s[i] - '0');
	}
	*out = v;
	return 0;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil), so no timegm() is needed. */
static int64_t days_from_civil(int64_t y, unsigned int m, unsigned int d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned int yoe = (unsigned int)(y - era * 400);
	const unsigned int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

/* The inverse, civil_from_days: no gmtime_r(), whose declaration depends on
 * the libc and its feature macros. */
void ztp_format_rfc3339(int64_t unix_s, char out[21])
{
	int64_t days = unix_s >= 0 ? unix_s / 86400 : (unix_s - 86399) / 86400;
	int64_t secs = unix_s - days * 86400;
	int64_t z = days + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned int doe = (unsigned int)(z - era * 146097);
	const unsigned int yoe =
		(doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const unsigned int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned int mp = (5 * doy + 2) / 153;
	const unsigned int d = doy - (153 * mp + 2) / 5 + 1;
	const unsigned int m = mp < 10 ? mp + 3 : mp - 9;
	const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);

	/* Via a scratch buffer: the compiler cannot prove the year has four
	 * digits, and a truncation it cannot rule out is a warning. */
	char tmp[48];

	snprintf(tmp, sizeof(tmp), "%04d-%02u-%02uT%02d:%02d:%02dZ", (int)y, m,
		 d, (int)(secs / 3600), (int)(secs / 60 % 60), (int)(secs % 60));
	memcpy(out, tmp, 20);
	out[20] = '\0';
}

int ztp_parse_rfc3339(const char *s, size_t len, int64_t *unix_s)
{
	int y, mo, d, h, mi, se;

	/* YYYY-MM-DDTHH:MM:SS then optional .fraction, then Z */
	if (len < 20 || s[4] != '-' || s[7] != '-' ||
	    (s[10] != 'T' && s[10] != 't') || s[13] != ':' || s[16] != ':' ||
	    (s[len - 1] != 'Z' && s[len - 1] != 'z')) {
		return -EINVAL;
	}
	if (len > 20 && s[19] != '.') {
		return -EINVAL; /* only UTC is produced by the server */
	}
	if (parse_uint(s, 4, &y) || parse_uint(s + 5, 2, &mo) ||
	    parse_uint(s + 8, 2, &d) || parse_uint(s + 11, 2, &h) ||
	    parse_uint(s + 14, 2, &mi) || parse_uint(s + 17, 2, &se)) {
		return -EINVAL;
	}
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 ||
	    se > 60) {
		return -EINVAL;
	}
	*unix_s = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
	return 0;
}

int ztp_response_parse(const char *buf, size_t len, struct ztp_response *r)
{
	const char *cur = buf;
	const char *end = buf + len;
	bool version_ok = false;
	struct ztp_kv kv;

	memset(r, 0, sizeof(*r));
	while (ztp_kv_next(&cur, end, &kv)) {
		if (ztp_slice_eq(kv.key, "protocol_version")) {
			version_ok = ztp_slice_eq(kv.val, "1");
		} else if (ztp_slice_eq(kv.key, "status")) {
			r->status = ztp_slice_eq(kv.val, "accepted") ? ZTP_STATUS_ACCEPTED
				  : ztp_slice_eq(kv.val, "pending")  ? ZTP_STATUS_PENDING
				  : ztp_slice_eq(kv.val, "rejected") ? ZTP_STATUS_REJECTED
								     : ZTP_STATUS_UNKNOWN;
		} else if (ztp_slice_eq(kv.key, "reason")) {
			r->reason = kv.val;
		} else if (ztp_slice_eq(kv.key, "retry_after")) {
			(void)parse_uint(kv.val.p, kv.val.len, &r->retry_after);
		} else if (ztp_slice_eq(kv.key, "server_time")) {
			(void)ztp_parse_rfc3339(kv.val.p, kv.val.len,
						&r->server_time);
		} else if (ztp_slice_eq(kv.key, "manifest.payload")) {
			r->manifest = kv.val;
		} else if (kv.key.len > 10 && memcmp(kv.key.p, "encrypted.", 10) == 0) {
			r->encrypted = true;
		}
		/* bundle.*, manifest.alg/key_id/signature and anything newer are
		 * ignored: the device trusts on first use and reads the manifest. */
	}
	if (!version_ok || r->status == ZTP_STATUS_UNKNOWN) {
		return -EPROTO;
	}
	return 0;
}

void ztp_manifest_header(const char *buf, size_t len,
			 struct ztp_manifest_hdr *hdr)
{
	const char *cur = buf;
	const char *end = buf + len;
	struct ztp_kv kv;

	memset(hdr, 0, sizeof(*hdr));
	while (ztp_kv_next(&cur, end, &kv)) {
		if (ztp_slice_eq(kv.key, "device_id")) {
			hdr->device_id = kv.val;
		} else if (ztp_slice_eq(kv.key, "protocol_version")) {
			hdr->version_ok = ztp_slice_eq(kv.val, "1");
		}
	}
}

int ztp_manifest_next_module(const char **cur, const char *end,
			     struct ztp_module *m)
{
	struct ztp_kv kv;

	while (ztp_kv_next(cur, end, &kv)) {
		struct ztp_slice rest = kv.val;

		memset(m, 0, sizeof(*m));
		if (ztp_slice_eq(kv.key, "module")) {
			/* module=<type> <base64 payload> */
			if (!next_field(&rest, &m->type) ||
			    !next_field(&rest, &m->payload)) {
				return -EPROTO;
			}
			return 1;
		}
		if (ztp_slice_eq(kv.key, "module-sealed")) {
			/* module-sealed=<type> <format> <eph> <nonce> <ciphertext> */
			m->sealed = true;
			if (!next_field(&rest, &m->type) ||
			    !next_field(&rest, &m->format) ||
			    !next_field(&rest, &m->eph) ||
			    !next_field(&rest, &m->nonce) ||
			    !next_field(&rest, &m->ciphertext)) {
				return -EPROTO;
			}
			return 1;
		}
	}
	return 0;
}
