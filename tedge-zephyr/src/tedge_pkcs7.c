/* SPDX-License-Identifier: Apache-2.0
 *
 * The EST reply is a PKCS#7 "certs-only" structure: walk its ASN.1 and take
 * the first certificate out of it, so no PKCS#7 parser is needed. Kept apart
 * from the enrollment flow because it is pure and unit-tested.
 */

#include "tedge_internal.h"

#include <mbedtls/asn1.h>
#include <mbedtls/base64.h>
#include <errno.h>
#include <string.h>

int tedge_pkcs7_first_cert(const char *b64, char *clean, size_t clean_cap,
			   uint8_t *der, size_t der_cap, uint8_t *out,
			   size_t cap, size_t *out_len)
{
	size_t n = 0, der_len;
	unsigned char *p, *end, *cert;
	size_t len;
	int ret;

	for (const char *s = b64; *s != '\0' && n < clean_cap - 1; s++) {
		if (*s != '\r' && *s != '\n' && *s != ' ') {
			clean[n++] = *s;
		}
	}
	clean[n] = '\0';
	ret = mbedtls_base64_decode(der, der_cap, &der_len,
				    (const unsigned char *)clean, n);
	if (ret != 0) {
		return -EINVAL;
	}

	p = der;
	end = der + der_len;
#define STEP(tag)                                                              \
	do {                                                                   \
		ret = mbedtls_asn1_get_tag(&p, end, &len, (tag));              \
		if (ret != 0) {                                                \
			return -EBADMSG;                                       \
		}                                                              \
	} while (0)
	/* ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT content } */
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	STEP(MBEDTLS_ASN1_OID);
	p += len;
	STEP(MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
	/* SignedData ::= SEQUENCE { version, digestAlgorithms SET,
	 *   encapContentInfo SEQUENCE, certificates [0] IMPLICIT ... } */
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	STEP(MBEDTLS_ASN1_INTEGER);
	p += len;
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET);
	p += len;
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	p += len;
	STEP(MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
	cert = p; /* the first Certificate, header included */
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
#undef STEP
	len += p - cert;
	if (len > cap) {
		return -ENOMEM;
	}
	memcpy(out, cert, len);
	*out_len = len;
	return 0;
}
