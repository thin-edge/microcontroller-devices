/* SPDX-License-Identifier: Apache-2.0
 *
 * A PKCS#7 "certs-only" reply, the shape Cumulocity's EST endpoint
 * returns, made with:
 *   openssl req -x509 -key ec.key -subj /CN=tedge-fixture -out cert.pem
 *   openssl crl2pkcs7 -nocrl -certfile cert.pem
 */

#ifndef PKCS7_FIXTURE_H_
#define PKCS7_FIXTURE_H_

static const char pkcs7_b64[] =
	"MIIBswYJKoZIhvcNAQcCoIIBpDCCAaACAQExADALBgkqhkiG9w0BBwGgggGIMIIB"
	"hDCCASugAwIBAgIUPvMW43vxKLSDaorLhsno7YZANh4wCgYIKoZIzj0EAwIwGDEW"
	"MBQGA1UEAwwNdGVkZ2UtZml4dHVyZTAeFw0yNjA5MTkyMTI0NTBaFw0zNjA5MTYy"
	"MTI0NTBaMBgxFjAUBgNVBAMMDXRlZGdlLWZpeHR1cmUwWTATBgcqhkjOPQIBBggq"
	"hkjOPQMBBwNCAATvPyoIttTV/hfOi3rPRHpZ3k3HbNUE9P3Pt7GV+qPN/ZeTv9H2"
	"bBffhqMKmG/UD7AklaLw+iMA7TnoavuQtR/co1MwUTAdBgNVHQ4EFgQUx1fE7Uu7"
	"aeNQE+APpYSdVFRy4icwHwYDVR0jBBgwFoAUx1fE7Uu7aeNQE+APpYSdVFRy4icw"
	"DwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiBByeANdK+0O8obbhDj"
	"1CpqJrA/IFFHfn4Z6B/SMrnv7QIgaIPBgABS9R0DTDuBqVayKdwIbwwhRYKc7ml1"
	"udaGeyMxAA==";

#define PKCS7_CERT_DER_LEN 392
/* The first bytes of the certificate: SEQUENCE header. */
static const unsigned char pkcs7_cert_head[] = { 0x30, 0x82, 0x01, 0x84, 0x30, 0x82, 0x01, 0x2b };

#endif /* PKCS7_FIXTURE_H_ */
