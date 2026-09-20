/* SPDX-License-Identifier: Apache-2.0
 *
 * Onboarding with the Cumulocity CA: the device makes its own key and
 * one-time password, shows a registration URL, and enrolls over EST
 * (simpleenroll) as soon as an operator registers the external ID.
 *
 *   key (PSA ITS) -> one-time password -> registration URL
 *     -> CSR signed inside PSA -> POST /.well-known/est/simpleenroll
 *        with Basic <external id>:<one-time password>
 *     -> PKCS#7 reply -> certificate -> TLS credentials
 *
 * The key never leaves PSA for signing the CSR. It is exported once, as DER,
 * because Zephyr's tls_credentials take a key buffer; that copy stays in RAM
 * while the credential is registered (a known limitation, spikes P9).
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>

#include <psa/crypto.h>
#include <mbedtls/asn1.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define KEY_ID ((psa_key_id_t)CONFIG_TEDGE_PSA_KEY_ID)
#define OTP_LEN 32
/* One call to tedge_auth_prepare() polls for about this long before it hands
 * control back to the core (which checks the network and calls again). */
#define ENROLL_WINDOW_MS (2 * 60 * 1000)
#define EST_PATH "/.well-known/est/simpleenroll"

/* Kept while the credential is registered. */
static uint8_t cert_der[1024];
static size_t cert_der_len;
static uint8_t key_der_buf[160];
static size_t key_der_len;
static const uint8_t *key_der;

static char otp[OTP_LEN + 1];
static bool credentials_ready;

/* Transient buffers from the module heap, only while enrolling. */
struct enroll_bufs {
	char csr_body[1024];
	char resp[4096];
	uint8_t http_rx[1024];
	uint8_t der[1536];
};
static struct enroll_bufs *bufs;
static size_t resp_len;
static uint16_t resp_status;

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

struct blob {
	void *buf;
	size_t cap;
	size_t len;
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct blob *b = param;

	ARG_UNUSED(key);
	if (len <= b->cap) {
		b->len = read_cb(cb_arg, b->buf, len);
	}
	return 0;
}

static size_t settings_get(const char *key, void *buf, size_t cap)
{
	struct blob b = { .buf = buf, .cap = cap };

	(void)settings_load_subtree_direct(key, load_cb, &b);
	return b.len;
}

/* ------------------------------------------------------------------------ */
/* Key and one-time password                                                 */
/* ------------------------------------------------------------------------ */

static int ensure_key(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	psa_status_t st;

	st = psa_get_key_attributes(KEY_ID, &attr);
	psa_reset_key_attributes(&attr);
	if (st == PSA_SUCCESS) {
		return 0; /* already generated on an earlier boot */
	}

	psa_set_key_id(&attr, KEY_ID);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH |
					       PSA_KEY_USAGE_SIGN_MESSAGE |
					       PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	st = psa_generate_key(&attr, &id);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		LOG_ERR("generating the device key failed (%d)", (int)st);
		return -EIO;
	}
	LOG_INF("device key generated (P-256, PSA key 0x%08x)",
		(unsigned int)KEY_ID);
	return 0;
}

static void ensure_otp(void)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	uint8_t rnd[OTP_LEN];

	if (otp[0] != '\0') {
		return;
	}
	if (settings_get(TEDGE_KEY_ENROLL_OTP, otp, OTP_LEN) == OTP_LEN) {
		otp[OTP_LEN] = '\0';
		return;
	}
	psa_generate_random(rnd, sizeof(rnd));
	for (size_t i = 0; i < OTP_LEN; i++) {
		otp[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
	}
	otp[OTP_LEN] = '\0';
	(void)settings_save_one(TEDGE_KEY_ENROLL_OTP, otp, OTP_LEN);
}

int tedge_registration_url(char *buf, size_t len)
{
	int n;

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}
	if (credentials_ready || cert_der_len > 0) {
		return -ENOENT; /* already enrolled */
	}
	ensure_otp();
	n = snprintf(buf, len,
		     "https://%s/apps/devicemanagement/index.html#/"
		     "deviceregistration?externalId=%s&one-time-password=%s",
		     tedge_c8y_host(), tedge_identity()->external_id, otp);
	return (n > 0 && (size_t)n < len) ? n : -ENOSPC;
}

/* The URL carries the one-time password, so it is only logged at debug
 * level; the application shows it through tedge_registration_url(). */
static void log_registration_hint(void)
{
	char url[220];

	if (tedge_registration_url(url, sizeof(url)) > 0) {
		LOG_INF("waiting to be registered as \"%s\" on %s",
			tedge_identity()->external_id, tedge_c8y_host());
		LOG_DBG("registration URL: %s", url);
	}
}

/* ------------------------------------------------------------------------ */
/* CSR                                                                       */
/* ------------------------------------------------------------------------ */

static int make_csr_internal(void);

/* Shared with renewal: build a CSR for the device key into the shared
 * buffer, and post it to an EST endpoint. */
int tedge_est_make_csr(char *out, size_t len)
{
	int ret;

	if (bufs == NULL) {
		bufs = tedge_alloc(sizeof(*bufs));
		if (bufs == NULL) {
			return -ENOMEM;
		}
	}
	ret = make_csr_internal();
	if (ret == 0 && out != NULL) {
		snprintf(out, len, "%s", bufs->csr_body);
	}
	return ret;
}

static int make_csr_internal(void)
{
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	char subject[80];
	unsigned char *pem = (unsigned char *)bufs->resp; /* reused as scratch */
	const size_t pem_cap = 1200;
	int ret;

	mbedtls_x509write_csr_init(&csr);
	mbedtls_pk_init(&pk);

	ret = mbedtls_pk_wrap_psa(&pk, KEY_ID);
	if (ret != 0) {
		LOG_ERR("could not use the PSA key for the CSR (-0x%04x)", -ret);
		goto out;
	}
	snprintf(subject, sizeof(subject), "CN=%s", tedge_identity()->external_id);
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &pk);
	ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
	if (ret == 0) {
		ret = mbedtls_x509write_csr_pem(&csr, pem, pem_cap);
	}
	if (ret != 0) {
		LOG_ERR("writing the CSR failed (-0x%04x)", -ret);
		goto out;
	}
	/* EST wants the base64 body without the PEM armour. */
	{
		const char *b = strstr((char *)pem, "-----\n");
		const char *e = strstr((char *)pem, "-----END");

		if (b == NULL || e == NULL) {
			ret = -EINVAL;
			goto out;
		}
		b += 6;
		snprintf(bufs->csr_body, sizeof(bufs->csr_body), "%.*s",
			 (int)(e - b), b);
	}
out:
	mbedtls_pk_free(&pk);
	mbedtls_x509write_csr_free(&csr);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* HTTPS POST to the tenant                                                  */
/* ------------------------------------------------------------------------ */

static int on_body(struct http_parser *parser, const char *at, size_t length)
{
	size_t n;

	ARG_UNUSED(parser);
	n = MIN(length, sizeof(bufs->resp) - 1 - resp_len);
	memcpy(bufs->resp + resp_len, at, n);
	resp_len += n;
	bufs->resp[resp_len] = '\0';
	return 0;
}

static const struct http_parser_settings parser_cb = { .on_body = on_body };

static int on_response(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	ARG_UNUSED(final);
	ARG_UNUSED(user_data);
	resp_status = rsp->http_status_code;
	return 0;
}

static int https_post(const char *path, const char *auth, const char *body)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	const char *headers[] = { auth, NULL };
	sec_tag_t tags[1] = { TEDGE_TAG_SERVER_CA };
	struct http_request req = { 0 };
	const char *host = tedge_c8y_host();
	int fd, ret;

	ret = zsock_getaddrinfo(host, "443", &hints, &res);
	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags,
			       sizeof(tags));
	(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, host,
			       strlen(host) + 1);
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret != 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}

	resp_len = 0;
	bufs->resp[0] = '\0';
	resp_status = 0;
	req.method = HTTP_POST;
	req.url = path;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.content_type_value = "application/pkcs10";
	req.payload = body;
	req.payload_len = strlen(body);
	req.header_fields = (auth != NULL) ? headers : NULL;
	req.response = on_response;
	req.http_cb = &parser_cb;
	req.recv_buf = bufs->http_rx;
	req.recv_buf_len = sizeof(bufs->http_rx);
	ret = http_client_req(fd, &req, 30000, NULL);
	zsock_close(fd);
	return (ret < 0) ? ret : 0;
}

/* ------------------------------------------------------------------------ */
/* PKCS#7 (certs-only) -> the DER certificate                                */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* TLS credentials                                                           */
/* ------------------------------------------------------------------------ */

static int register_credentials(void)
{
	mbedtls_pk_context pk;
	int ret;

	if (credentials_ready) {
		return 0;
	}
	mbedtls_pk_init(&pk);
	ret = mbedtls_pk_copy_from_psa(KEY_ID, &pk);
	if (ret == 0) {
		ret = mbedtls_pk_write_key_der(&pk, key_der_buf,
					       sizeof(key_der_buf));
	}
	mbedtls_pk_free(&pk);
	if (ret <= 0) {
		LOG_ERR("exporting the device key failed (-0x%04x)", -ret);
		return -EIO;
	}
	/* mbedtls_pk_write_key_der writes at the end of the buffer. */
	key_der_len = ret;
	key_der = key_der_buf + sizeof(key_der_buf) - key_der_len;

	(void)tls_credential_delete(TEDGE_TAG_DEVICE,
				    TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
	(void)tls_credential_delete(TEDGE_TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY);
	ret = tls_credential_add(TEDGE_TAG_DEVICE,
				 TLS_CREDENTIAL_PUBLIC_CERTIFICATE, cert_der,
				 cert_der_len);
	if (ret == 0) {
		ret = tls_credential_add(TEDGE_TAG_DEVICE,
					 TLS_CREDENTIAL_PRIVATE_KEY, key_der,
					 key_der_len);
	}
	if (ret != 0) {
		LOG_ERR("registering the device credentials failed (%d)", ret);
		return ret;
	}
	credentials_ready = true;
	LOG_INF("device certificate ready (%zu B)", cert_der_len);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Enrollment                                                                */
/* ------------------------------------------------------------------------ */

/* Shared with renewal: POST the CSR in the shared buffer to @p path with
 * @p auth, and unwrap the certificate from the reply into @p out.
 */
int tedge_est_request(const char *path, const char *auth, uint8_t *out,
		      size_t cap, size_t *out_len)
{
	int ret;

	if (bufs == NULL) {
		return -EINVAL;
	}
	ret = https_post(path, auth, bufs->csr_body);
	if (ret != 0) {
		return ret;
	}
	if (resp_status != 200) {
		LOG_WRN("EST %s: the server answered %u", path, resp_status);
		return -EACCES;
	}
	return tedge_pkcs7_first_cert(bufs->resp, bufs->csr_body,
				      sizeof(bufs->csr_body), bufs->der,
				      sizeof(bufs->der), out, cap, out_len);
}

/* Shared with renewal: drop the transient buffers once a flow is done. */
void tedge_est_release(void)
{
	tedge_free(bufs);
	bufs = NULL;
}

/* Shared with renewal: store a new certificate and use it from now on. */
int tedge_credentials_replace(const uint8_t *der, size_t len)
{
	if (len == 0 || len > sizeof(cert_der)) {
		return -EINVAL;
	}
	memcpy(cert_der, der, len);
	cert_der_len = len;
	(void)settings_save_one(TEDGE_KEY_ENROLL_CERT, cert_der, cert_der_len);
	credentials_ready = false; /* re-register under the same tag */
	return register_credentials();
}

/* The certificate in use, for the expiry check. */
const uint8_t *tedge_credentials_cert(size_t *len)
{
	*len = cert_der_len;
	return (cert_der_len > 0) ? cert_der : NULL;
}

static int enroll_once(void)
{
	char basic[128];
	char basic_b64[180];
	char auth_hdr[220];
	size_t olen;
	int ret;

	snprintf(basic, sizeof(basic), "%s:%s", tedge_identity()->external_id,
		 otp);
	ret = mbedtls_base64_encode((unsigned char *)basic_b64,
				    sizeof(basic_b64), &olen,
				    (const unsigned char *)basic, strlen(basic));
	if (ret != 0) {
		return -EINVAL;
	}
	basic_b64[olen] = '\0';
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s\r\n",
		 basic_b64);

	ret = https_post(EST_PATH, auth_hdr, bufs->csr_body);
	if (ret != 0) {
		LOG_WRN("enrollment request failed (%d)", ret);
		return ret;
	}
	if (resp_status != 200) {
		/* 401 until an operator registers the external ID. */
		LOG_DBG("not registered yet (HTTP %u)", resp_status);
		return -EAGAIN;
	}
	ret = tedge_pkcs7_first_cert(bufs->resp, bufs->csr_body,
				     sizeof(bufs->csr_body), bufs->der,
				     sizeof(bufs->der), cert_der,
				     sizeof(cert_der), &cert_der_len);
	if (ret != 0) {
		LOG_ERR("could not read the certificate from the reply (%d)", ret);
		return ret;
	}
	(void)settings_save_one(TEDGE_KEY_ENROLL_CERT, cert_der, cert_der_len);
	(void)settings_delete(TEDGE_KEY_ENROLL_OTP);
	memset(otp, 0, sizeof(otp));
	LOG_INF("enrolled: certificate stored (%zu B)", cert_der_len);
	return 0;
}

int tedge_auth_prepare(char *id_out, size_t id_len)
{
	int64_t deadline;
	uint32_t wait_s = CONFIG_TEDGE_ENROLL_POLL_S;
	int ret;

	snprintf(id_out, id_len, "%s", tedge_identity()->external_id);
	if (credentials_ready) {
		return 0;
	}

	if (cert_der_len == 0) {
		cert_der_len = settings_get(TEDGE_KEY_ENROLL_CERT, cert_der,
					    sizeof(cert_der));
	}
	ret = ensure_key();
	if (ret != 0) {
		return ret;
	}
	if (cert_der_len > 0) {
		return register_credentials();
	}

	/* Enrolling: the transient buffers come from the module heap. */
	bufs = tedge_alloc(sizeof(*bufs));
	if (bufs == NULL) {
		return -ENOMEM;
	}
	ensure_otp();
	tedge_set_state(TEDGE_STATE_AWAITING_REGISTRATION);
	log_registration_hint();

	ret = make_csr_internal();
	deadline = k_uptime_get() + ENROLL_WINDOW_MS;
	while (ret == 0 && k_uptime_get() < deadline) {
		ret = enroll_once();
		if (ret == 0) {
			break;
		}
		k_sleep(K_SECONDS(wait_s));
		wait_s = MIN(wait_s * 2, 60U);
		ret = -EAGAIN;
	}
	if (ret == 0) {
		ret = register_credentials();
	}
	tedge_free(bufs);
	bufs = NULL;
	return ret;
}

int tedge_set_bootstrap_credentials(const char *user, const char *password)
{
	ARG_UNUSED(user);
	ARG_UNUSED(password);
	return -ENOTSUP; /* this build authenticates with a certificate */
}

const char *tedge_auth_username(void)
{
	return NULL; /* mutual TLS */
}

const char *tedge_auth_password(void)
{
	return NULL;
}
