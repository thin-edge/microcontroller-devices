/* SPDX-License-Identifier: Apache-2.0
 *
 * Minimal ASN.1/BER codec for SNMPv2c — just the types the switch/router MIB
 * needs. Two independent halves:
 *
 *  - Encoder (backward): builds a message from the end of a fixed buffer toward
 *    the front, which makes TLV length/nesting trivial (children are written
 *    before their SEQUENCE header). No dynamic allocation; overflow is reported,
 *    never written past the buffer.
 *  - Decoder (forward): walks a received datagram TLV by TLV, fully
 *    bounds-checked, so a malformed PDU is rejected rather than read out of range.
 */
#ifndef APP_SNMP_BER_H_
#define APP_SNMP_BER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Universal / application / context tags used by SNMPv2c. */
#define BER_TAG_INTEGER   0x02
#define BER_TAG_OCTET_STR 0x04
#define BER_TAG_NULL      0x05
#define BER_TAG_OID       0x06
#define BER_TAG_SEQUENCE  0x30 /* constructed */

#define BER_TAG_IPADDRESS 0x40 /* [APPLICATION 0], 4 octets */
#define BER_TAG_COUNTER32 0x41 /* [APPLICATION 1] */
#define BER_TAG_GAUGE32   0x42 /* [APPLICATION 2] */
#define BER_TAG_TIMETICKS 0x43 /* [APPLICATION 3] */

/* SNMPv2c PDU tags ([CONTEXT n] constructed). */
#define SNMP_PDU_GET      0xA0
#define SNMP_PDU_GETNEXT  0xA1
#define SNMP_PDU_RESPONSE 0xA2
#define SNMP_PDU_SET      0xA3
#define SNMP_PDU_GETBULK  0xA5

/* SNMPv2c varbind exception values (context primitive, zero length). */
#define SNMP_EXC_NO_SUCH_OBJECT   0x80
#define SNMP_EXC_NO_SUCH_INSTANCE 0x81
#define SNMP_EXC_END_OF_MIB_VIEW  0x82

/* Longest OID (in sub-identifiers) we parse or store. */
#define BER_MAX_OID_LEN 32

/* --- Backward encoder --- */

struct ber_enc {
	uint8_t *base; /* buffer start */
	size_t cap;    /* buffer size */
	size_t pos;    /* next write index; content occupies [pos, cap) */
	bool overflow; /* set once a write did not fit */
};

/** Initialise an encoder over [buf, buf+cap); writes grow downward from cap. */
void ber_enc_init(struct ber_enc *e, uint8_t *buf, size_t cap);

/** @return pointer to the encoded message (into the caller's buffer). */
static inline const uint8_t *ber_enc_data(const struct ber_enc *e)
{
	return e->base + e->pos;
}

/** @return encoded length in bytes (valid only if !e->overflow). */
size_t ber_enc_length(const struct ber_enc *e);

/**
 * Mark the current end position (call before writing an element's children;
 * pass the returned value to ber_wrap() to add the SEQUENCE/constructed header).
 */
static inline size_t ber_mark(const struct ber_enc *e)
{
	return e->pos;
}

/** Wrap already-written children [pos, mark) in a length + tag header. */
void ber_wrap(struct ber_enc *e, size_t mark, uint8_t tag);

/* Leaf writers — each prepends a full TLV. All are no-ops once overflow is set. */
void ber_put_int(struct ber_enc *e, int32_t v);
void ber_put_uint_tagged(struct ber_enc *e, uint32_t v, uint8_t tag);
void ber_put_octet_str(struct ber_enc *e, const uint8_t *s, size_t n);
void ber_put_oid(struct ber_enc *e, const uint32_t *subids, size_t n);
void ber_put_null(struct ber_enc *e);
void ber_put_exception(struct ber_enc *e, uint8_t exc_tag);
void ber_put_ipaddress(struct ber_enc *e, uint32_t addr_be);

/* --- Forward decoder --- */

struct ber_dec {
	const uint8_t *p;
	const uint8_t *end;
};

/** Initialise a decoder over [buf, buf+len). */
void ber_dec_init(struct ber_dec *d, const uint8_t *buf, size_t len);

/** @return true if the decoder has consumed all input. */
static inline bool ber_dec_done(const struct ber_dec *d)
{
	return d->p >= d->end;
}

/**
 * Read the next TLV header. On success sets *tag, *content (into the input), and
 * *clen, and advances the decoder past the value. @return false on truncation.
 */
bool ber_get_tlv(struct ber_dec *d, uint8_t *tag, const uint8_t **content,
		 size_t *clen);

/**
 * Read a TLV expected to be `tag` (constructed or primitive) and open a nested
 * decoder `inner` over its content, advancing `d` past it. @return false on
 * mismatch or truncation.
 */
bool ber_enter(struct ber_dec *d, uint8_t tag, struct ber_dec *inner);

/** Read an INTEGER into a signed 32-bit value. */
bool ber_get_int(struct ber_dec *d, int32_t *v);

/** Read an OCTET STRING; sets *s (into the input) and *n. */
bool ber_get_octet(struct ber_dec *d, const uint8_t **s, size_t *n);

/** Read an OID into out[0..*n); *n bounded by max. */
bool ber_get_oid(struct ber_dec *d, uint32_t *out, size_t max, size_t *n);

/** Skip the next TLV (any type). @return false on truncation. */
bool ber_skip(struct ber_dec *d);

/* --- OID helpers --- */

/** Lexicographic compare of two OIDs. <0, 0, >0 like memcmp. */
int oid_cmp(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen);

#endif /* APP_SNMP_BER_H_ */
