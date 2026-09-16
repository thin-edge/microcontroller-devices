/* SPDX-License-Identifier: Apache-2.0
 *
 * Minimal ASN.1/BER codec for SNMPv2c. See snmp_ber.h for the model (backward
 * encoder, forward decoder). Only definite-length, low-tag-number forms are
 * used, which is all SNMP requires.
 */

#include "snmp_ber.h"

#include <string.h>

/* ======================= Backward encoder ======================= */

void ber_enc_init(struct ber_enc *e, uint8_t *buf, size_t cap)
{
	e->base = buf;
	e->cap = cap;
	e->pos = cap;
	e->overflow = false;
}

size_t ber_enc_length(const struct ber_enc *e)
{
	return e->overflow ? 0 : (e->cap - e->pos);
}

/* Prepend one byte. */
static void put_byte(struct ber_enc *e, uint8_t b)
{
	if (e->overflow || e->pos == 0) {
		e->overflow = true;
		return;
	}
	e->base[--e->pos] = b;
}

/* Prepend n bytes preserving their order (src[0] ends up lowest). */
static void put_bytes(struct ber_enc *e, const uint8_t *src, size_t n)
{
	for (size_t i = n; i-- > 0;) {
		put_byte(e, src[i]);
	}
}

/* Prepend a definite length. */
static void put_len(struct ber_enc *e, size_t len)
{
	if (len < 0x80) {
		put_byte(e, (uint8_t)len);
		return;
	}

	uint8_t tmp[5];
	size_t n = 0;

	while (len > 0) {
		tmp[n++] = (uint8_t)(len & 0xFF);
		len >>= 8;
	}
	/* tmp holds little-endian; emit big-endian then the count byte. */
	for (size_t i = 0; i < n; i++) {
		put_byte(e, tmp[i]);
	}
	put_byte(e, (uint8_t)(0x80 | n));
}

void ber_wrap(struct ber_enc *e, size_t mark, uint8_t tag)
{
	size_t clen = mark - e->pos;

	put_len(e, clen);
	put_byte(e, tag);
}

void ber_put_int(struct ber_enc *e, int32_t v)
{
	uint8_t tmp[4];
	int n = 0;
	/* Minimal two's-complement big-endian encoding. */
	uint32_t u = (uint32_t)v;

	do {
		tmp[n++] = (uint8_t)(u & 0xFF);
		u = (uint32_t)((int32_t)u >> 8); /* arithmetic shift for sign */
	} while (!((u == 0 && !(tmp[n - 1] & 0x80)) ||
		   (u == 0xFFFFFFFF && (tmp[n - 1] & 0x80))));

	/* tmp[0..n) little-endian; emit big-endian. */
	for (int i = 0; i < n; i++) {
		put_byte(e, tmp[i]);
	}
	put_len(e, (size_t)n);
	put_byte(e, BER_TAG_INTEGER);
}

void ber_put_uint_tagged(struct ber_enc *e, uint32_t v, uint8_t tag)
{
	uint8_t tmp[5];
	int n = 0;

	/* Unsigned big-endian, minimal, with a leading 0x00 if the MSB is set. */
	do {
		tmp[n++] = (uint8_t)(v & 0xFF);
		v >>= 8;
	} while (v > 0);

	if (tmp[n - 1] & 0x80) {
		tmp[n++] = 0x00;
	}

	for (int i = 0; i < n; i++) {
		put_byte(e, tmp[i]);
	}
	put_len(e, (size_t)n);
	put_byte(e, tag);
}

void ber_put_octet_str(struct ber_enc *e, const uint8_t *s, size_t n)
{
	put_bytes(e, s, n);
	put_len(e, n);
	put_byte(e, BER_TAG_OCTET_STR);
}

void ber_put_oid(struct ber_enc *e, const uint32_t *subids, size_t n)
{
	/* Encode from the last sub-identifier back to the first (backward emit). */
	if (n < 2) {
		/* Not a valid OID; emit an empty one to stay well-formed. */
		put_len(e, 0);
		put_byte(e, BER_TAG_OID);
		return;
	}

	size_t start = e->pos; /* to measure content length after emitting */

	for (size_t i = n; i-- > 2;) {
		uint32_t v = subids[i];
		uint8_t tmp[5];
		int m = 0;

		tmp[m++] = (uint8_t)(v & 0x7F);
		v >>= 7;
		while (v > 0) {
			tmp[m++] = (uint8_t)((v & 0x7F) | 0x80);
			v >>= 7;
		}
		/* tmp[0] is the low 7 bits (no continuation); higher indices carry
		 * the continuation bit. The wire order is high-order byte first
		 * (tmp[m-1]..tmp[0]); since put_byte() prepends, iterate tmp[0]..tmp[m-1]
		 * so the group lands in that order. */
		for (int k = 0; k < m; k++) {
			put_byte(e, tmp[k]);
		}
	}

	/* First byte combines the first two arcs. */
	uint32_t first = subids[0] * 40 + subids[1];
	uint8_t tmp[5];
	int m = 0;

	tmp[m++] = (uint8_t)(first & 0x7F);
	first >>= 7;
	while (first > 0) {
		tmp[m++] = (uint8_t)((first & 0x7F) | 0x80);
		first >>= 7;
	}
	/* Same prepend ordering as the sub-identifier loop above. */
	for (int k = 0; k < m; k++) {
		put_byte(e, tmp[k]);
	}

	size_t clen = start - e->pos;

	put_len(e, clen);
	put_byte(e, BER_TAG_OID);
}

void ber_put_null(struct ber_enc *e)
{
	put_byte(e, 0x00);
	put_byte(e, BER_TAG_NULL);
}

void ber_put_exception(struct ber_enc *e, uint8_t exc_tag)
{
	put_byte(e, 0x00);
	put_byte(e, exc_tag);
}

void ber_put_ipaddress(struct ber_enc *e, uint32_t addr_be)
{
	uint8_t b[4];

	memcpy(b, &addr_be, 4); /* addr_be already in network order */
	put_bytes(e, b, 4);
	put_len(e, 4);
	put_byte(e, BER_TAG_IPADDRESS);
}

/* ======================= Forward decoder ======================= */

void ber_dec_init(struct ber_dec *d, const uint8_t *buf, size_t len)
{
	d->p = buf;
	d->end = buf + len;
}

bool ber_get_tlv(struct ber_dec *d, uint8_t *tag, const uint8_t **content,
		 size_t *clen)
{
	if (d->p >= d->end) {
		return false;
	}

	uint8_t t = *d->p++;

	if (d->p >= d->end) {
		return false;
	}

	uint8_t l0 = *d->p++;
	size_t len;

	if (l0 < 0x80) {
		len = l0;
	} else {
		int nbytes = l0 & 0x7F;

		if (nbytes == 0 || nbytes > 4 || (d->p + nbytes) > d->end) {
			return false; /* indefinite or absurd length */
		}
		len = 0;
		for (int i = 0; i < nbytes; i++) {
			len = (len << 8) | *d->p++;
		}
	}

	if ((size_t)(d->end - d->p) < len) {
		return false; /* truncated value */
	}

	*tag = t;
	*content = d->p;
	*clen = len;
	d->p += len;
	return true;
}

bool ber_enter(struct ber_dec *d, uint8_t tag, struct ber_dec *inner)
{
	uint8_t t;
	const uint8_t *c;
	size_t n;

	if (!ber_get_tlv(d, &t, &c, &n) || t != tag) {
		return false;
	}
	ber_dec_init(inner, c, n);
	return true;
}

bool ber_get_int(struct ber_dec *d, int32_t *v)
{
	uint8_t t;
	const uint8_t *c;
	size_t n;

	if (!ber_get_tlv(d, &t, &c, &n) || t != BER_TAG_INTEGER || n == 0 || n > 5) {
		return false;
	}

	/* Sign-extend from the first byte. */
	int64_t val = (c[0] & 0x80) ? -1 : 0;

	for (size_t i = 0; i < n; i++) {
		val = (val << 8) | c[i];
	}
	*v = (int32_t)val;
	return true;
}

bool ber_get_octet(struct ber_dec *d, const uint8_t **s, size_t *n)
{
	uint8_t t;

	if (!ber_get_tlv(d, &t, s, n) || t != BER_TAG_OCTET_STR) {
		return false;
	}
	return true;
}

bool ber_get_oid(struct ber_dec *d, uint32_t *out, size_t max, size_t *n)
{
	uint8_t t;
	const uint8_t *c;
	size_t clen;

	if (!ber_get_tlv(d, &t, &c, &clen) || t != BER_TAG_OID || clen == 0) {
		return false;
	}

	size_t cnt = 0;
	size_t i = 0;

	/* First byte encodes the first two arcs. */
	uint32_t first = c[i++];

	if (cnt + 2 > max) {
		return false;
	}
	out[cnt++] = first / 40;
	out[cnt++] = first % 40;

	while (i < clen) {
		uint32_t v = 0;

		do {
			if (i >= clen) {
				return false; /* dangling continuation */
			}
			v = (v << 7) | (c[i] & 0x7F);
		} while (c[i++] & 0x80);

		if (cnt >= max) {
			return false;
		}
		out[cnt++] = v;
	}

	*n = cnt;
	return true;
}

bool ber_skip(struct ber_dec *d)
{
	uint8_t t;
	const uint8_t *c;
	size_t n;

	return ber_get_tlv(d, &t, &c, &n);
}

int oid_cmp(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen)
{
	size_t n = (alen < blen) ? alen : blen;

	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return (a[i] < b[i]) ? -1 : 1;
		}
	}
	if (alen == blen) {
		return 0;
	}
	return (alen < blen) ? -1 : 1;
}
