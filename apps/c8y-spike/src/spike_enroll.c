/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike C (c8y-direct-spikes, tasks 5.1-5.6): Cumulocity CA enrollment on the
 * device. Throwaway measurement code; the production feature goes into
 * tedge-zephyr.
 *
 * - A persistent P-256 key in PSA ITS (secure storage over settings).
 * - A 32-character one-time password and the pre-filled registration URL.
 * - A PKCS#10 CSR (CN = external ID) signed with the PSA key, which never
 *   leaves PSA for this.
 * - POST /.well-known/est/simpleenroll with Basic <id>:<otp>, polled until
 *   200; the base64 PKCS#7 (certs-only) reply is unwrapped to the DER
 *   certificate and stored.
 * - The TLS credentials for mutual TLS: the certificate, and the key exported
 *   into a RAM buffer (Zephyr's tls_credentials take no PSA key ID).
 * - `spike enroll reenroll`: simplereenroll with mTLS only, then with a
 *   Bearer JWT (task 5.6).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include <psa/crypto.h>
#include <mbedtls/asn1.h>
#include <mbedtls/base64.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>

#include "spike.h"

LOG_MODULE_REGISTER(spike_enroll, LOG_LEVEL_INF);

/* In the application range, PSA_KEY_ID_USER_MIN..PSA_KEY_ID_USER_MAX
 * (1..0x3FFFFFFF); IDs above it are for the vendor. */
#define KEY_ID ((psa_key_id_t)0x0007E571)

static char device_id[48];
static char otp[33];
static uint8_t cert_der[1024];
static size_t cert_der_len;
/* The private key as DER, for tls_credentials. It stays in RAM for as long
 * as the credential is registered (U9).
 */
static uint8_t key_der_buf[160];
static const uint8_t *key_der;
static size_t key_der_len;

static char csr_body[1024];
static char resp[4096];
static size_t resp_len;
static uint16_t resp_status;
static uint8_t http_rx[1024];

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
	struct blob b = {.buf = buf, .cap = cap};

	settings_load_subtree_direct(key, load_cb, &b);
	return b.len;
}

/* ------------------------------------------------------------------------ */
/* Identity, key, one-time password                                          */
/* ------------------------------------------------------------------------ */

static void make_device_id(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *ll = iface ? net_if_get_link_addr(iface) : NULL;
	size_t off;

	if (CONFIG_SPIKE_ENROLL_DEVICE_ID[0]) {
		snprintk(device_id, sizeof(device_id), "%s",
			 CONFIG_SPIKE_ENROLL_DEVICE_ID);
		return;
	}
	off = snprintk(device_id, sizeof(device_id), "tedge-");
	for (size_t i = 0; ll && i < ll->len && off + 2 < sizeof(device_id); i++) {
		off += snprintk(device_id + off, sizeof(device_id) - off, "%02x",
				ll->addr[i]);
	}
}

static int ensure_key(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	int64_t t0 = k_uptime_get();
	psa_status_t st;

	st = psa_get_key_attributes(KEY_ID, &attr);
	psa_reset_key_attributes(&attr);
	if (st == PSA_SUCCESS) {
		LOG_INF("key: persistent P-256 key 0x%08x found in PSA ITS",
			(unsigned int)KEY_ID);
		return 0;
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
		LOG_ERR("psa_generate_key failed: %d", st);
		return -EIO;
	}
	LOG_INF("MEAS key: generated persistent P-256 key 0x%08x in %lld ms",
		(unsigned int)id, k_uptime_get() - t0);
	return 0;
}

static void ensure_otp(void)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	uint8_t rnd[32];

	if (settings_get("spike/enroll/otp", otp, sizeof(otp) - 1) == 32) {
		otp[32] = '\0';
		return;
	}
	psa_generate_random(rnd, sizeof(rnd));
	for (size_t i = 0; i < 32; i++) {
		/* 62 symbols; the tiny modulo bias is fine for a one-time use. */
		otp[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
	}
	otp[32] = '\0';
	settings_save_one("spike/enroll/otp", otp, 32);
}

static void print_registration_url(void)
{
	LOG_INF("REGISTER https://%s/apps/devicemanagement/index.html#/"
		"deviceregistration?externalId=%s&one-time-password=%s",
		CONFIG_SPIKE_C8Y_HOST, device_id, otp);
}

/* ------------------------------------------------------------------------ */
/* CSR                                                                       */
/* ------------------------------------------------------------------------ */

static int make_csr(void)
{
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	static unsigned char pem[1200];
	char subject[64];
	int64_t t0 = k_uptime_get();
	int ret;

	mbedtls_x509write_csr_init(&csr);
	mbedtls_pk_init(&pk);

	ret = mbedtls_pk_wrap_psa(&pk, KEY_ID);
	if (ret) {
		LOG_ERR("mbedtls_pk_wrap_psa: -0x%04x", -ret);
		goto out;
	}
	snprintk(subject, sizeof(subject), "CN=%s", device_id);
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &pk);
	ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
	if (ret) {
		LOG_ERR("CSR subject: -0x%04x", -ret);
		goto out;
	}
	ret = mbedtls_x509write_csr_pem(&csr, pem, sizeof(pem));
	if (ret) {
		LOG_ERR("CSR write: -0x%04x", -ret);
		goto out;
	}

	/* EST wants the base64 body without the armour lines (as tedge sends). */
	{
		const char *b = strstr((char *)pem, "-----\n");
		const char *e = strstr((char *)pem, "-----END");

		if (!b || !e) {
			ret = -EINVAL;
			goto out;
		}
		b += 6;
		snprintk(csr_body, sizeof(csr_body), "%.*s", (int)(e - b), b);
	}
	LOG_INF("MEAS csr: %zu-byte body (CN=%s) signed in %lld ms",
		strlen(csr_body), device_id, k_uptime_get() - t0);
	/* One line per 64 characters, to copy into `openssl req -verify`. */
	LOG_INF("CSR PEM follows:");
	for (const char *p = (const char *)pem; *p;) {
		const char *nl = strchr(p, '\n');
		int n = nl ? (int)(nl - p) : (int)strlen(p);

		LOG_INF("CSR| %.*s", n, p);
		k_msleep(5); /* pace the burst so the log buffer keeps up */
		p += n + (nl ? 1 : 0);
	}
out:
	mbedtls_pk_free(&pk);
	mbedtls_x509write_csr_free(&csr);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* HTTPS POST                                                                */
/* ------------------------------------------------------------------------ */

static int on_body(struct http_parser *parser, const char *at, size_t length)
{
	ARG_UNUSED(parser);
	size_t n = MIN(length, sizeof(resp) - 1 - resp_len);

	memcpy(resp + resp_len, at, n);
	resp_len += n;
	resp[resp_len] = '\0';
	return 0;
}

static const struct http_parser_settings parser_cb = {.on_body = on_body};

static int on_response(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	resp_status = rsp->http_status_code;
	return 0;
}

/**
 * POST @p body to https://<tenant>@p path. @p auth is a full header line
 * ("Authorization: ...\r\n") or NULL. With @p mtls, the device certificate is
 * offered in the handshake.
 */
static int https_post(const char *path, const char *auth, bool mtls,
		      const char *body)
{
	struct zsock_addrinfo hints = {.ai_family = AF_INET,
				       .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res;
	const char *headers[] = {auth, NULL};
	sec_tag_t tags[2] = {SPIKE_TAG_SERVER_CA, SPIKE_TAG_DEVICE};
	struct http_request req = {0};
	int fd, ret;

	ret = zsock_getaddrinfo(CONFIG_SPIKE_C8Y_HOST, "443", &hints, &res);
	if (ret) {
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}
	zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags,
			 (mtls ? 2 : 1) * sizeof(sec_tag_t));
	zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME,
			 CONFIG_SPIKE_C8Y_HOST, sizeof(CONFIG_SPIKE_C8Y_HOST));
	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}

	resp_len = 0;
	resp[0] = '\0';
	resp_status = 0;
	req.method = HTTP_POST;
	req.url = path;
	req.host = CONFIG_SPIKE_C8Y_HOST;
	req.protocol = "HTTP/1.1";
	req.content_type_value = "application/pkcs10";
	req.payload = body;
	req.payload_len = strlen(body);
	req.header_fields = auth ? headers : NULL;
	req.response = on_response;
	req.http_cb = &parser_cb;
	req.recv_buf = http_rx;
	req.recv_buf_len = sizeof(http_rx);
	ret = http_client_req(fd, &req, 30000, NULL);
	zsock_close(fd);
	return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------------ */
/* PKCS#7 certs-only -> DER certificate                                      */
/* ------------------------------------------------------------------------ */

static int unwrap_pkcs7(const char *b64, uint8_t *out, size_t cap, size_t *out_len)
{
	static uint8_t der[1536];
	static char clean[2048];
	size_t n = 0, der_len;
	unsigned char *p, *end, *cert;
	size_t len;
	int ret;

	for (const char *s = b64; *s && n < sizeof(clean) - 1; s++) {
		if (*s != '\r' && *s != '\n' && *s != ' ') {
			clean[n++] = *s;
		}
	}
	clean[n] = '\0';
	ret = mbedtls_base64_decode(der, sizeof(der), &der_len,
				    (const unsigned char *)clean, n);
	if (ret) {
		return -EINVAL;
	}

	p = der;
	end = der + der_len;
#define STEP(tag) do { ret = mbedtls_asn1_get_tag(&p, end, &len, (tag)); \
		if (ret) { return -EBADMSG; } } while (0)
	/* ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT content } */
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	STEP(MBEDTLS_ASN1_OID);
	p += len;
	STEP(MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
	/* SignedData ::= SEQUENCE { version, digestAlgorithms SET,
	 *   encapContentInfo SEQUENCE, certificates [0] IMPLICIT ... }
	 */
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	STEP(MBEDTLS_ASN1_INTEGER);
	p += len;
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET);
	p += len;
	STEP(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
	p += len;
	STEP(MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
	/* The first Certificate ::= SEQUENCE, header included. */
	cert = p;
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

/* ------------------------------------------------------------------------ */
/* TLS credentials                                                           */
/* ------------------------------------------------------------------------ */

static int register_credentials(void)
{
	mbedtls_pk_context pk;
	int ret;

	mbedtls_pk_init(&pk);
	ret = mbedtls_pk_copy_from_psa(KEY_ID, &pk);
	if (ret == 0) {
		ret = mbedtls_pk_write_key_der(&pk, key_der_buf, sizeof(key_der_buf));
	}
	mbedtls_pk_free(&pk);
	if (ret <= 0) {
		LOG_ERR("exporting the key failed: -0x%04x", -ret);
		return -EIO;
	}
	/* pk_write_key_der writes at the end of the buffer. */
	key_der_len = ret;
	key_der = key_der_buf + sizeof(key_der_buf) - key_der_len;

	tls_credential_delete(SPIKE_TAG_DEVICE, TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
	tls_credential_delete(SPIKE_TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY);
	ret = tls_credential_add(SPIKE_TAG_DEVICE, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 cert_der, cert_der_len);
	ret = ret ?: tls_credential_add(SPIKE_TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY,
					key_der, key_der_len);
	LOG_INF("MEAS credentials: certificate %zu B, exported key %zu B held in "
		"RAM while registered (%d)", cert_der_len, key_der_len, ret);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Enrollment                                                                */
/* ------------------------------------------------------------------------ */

int spike_enroll_run(char *id_out, size_t id_len)
{
	char auth_hdr[192];
	char basic[96];
	char basic_b64[140];
	size_t olen;
	int64_t t_start;
	int ret, attempt = 0;

	settings_subsys_init();
	make_device_id();
	snprintk(id_out, id_len, "%s", device_id);
	LOG_INF("enroll: external ID %s", device_id);

	ret = ensure_key();
	if (ret) {
		return ret;
	}

	cert_der_len = settings_get("spike/enroll/cert", cert_der, sizeof(cert_der));
	if (cert_der_len > 0) {
		LOG_INF("enroll: certificate found in settings (%zu B)", cert_der_len);
		return register_credentials();
	}

	ensure_otp();
	print_registration_url();
	ret = make_csr();
	if (ret) {
		return ret;
	}

	snprintk(basic, sizeof(basic), "%s:%s", device_id, otp);
	mbedtls_base64_encode((unsigned char *)basic_b64, sizeof(basic_b64), &olen,
			      (const unsigned char *)basic, strlen(basic));
	basic_b64[olen] = '\0';
	snprintk(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s\r\n",
		 basic_b64);

	t_start = k_uptime_get();
	for (;;) {
		attempt++;
		ret = https_post("/.well-known/est/simpleenroll", auth_hdr, false,
				 csr_body);
		if (ret == 0 && resp_status == 200) {
			break;
		}
		LOG_INF("enroll: attempt %d: %s HTTP %u %.*s", attempt,
			ret ? "request failed" : "not registered yet:", resp_status,
			(int)MIN(resp_len, 160), resp);
		if (attempt == 1 || attempt % 6 == 0) {
			print_registration_url();
		}
		k_sleep(K_SECONDS(10));
	}
	LOG_INF("MEAS enroll: certificate issued after %d attempts, %lld ms after "
		"the first; reply %zu B", attempt, k_uptime_get() - t_start,
		resp_len);

	ret = unwrap_pkcs7(resp, cert_der, sizeof(cert_der), &cert_der_len);
	if (ret) {
		LOG_ERR("unwrapping the PKCS#7 reply failed: %d", ret);
		return ret;
	}
	settings_save_one("spike/enroll/cert", cert_der, cert_der_len);
	settings_delete("spike/enroll/otp");
	LOG_INF("enroll: certificate stored (%zu B DER)", cert_der_len);
	return register_credentials();
}

/* ------------------------------------------------------------------------ */
/* simplereenroll (task 5.6) and the shell                                   */
/* ------------------------------------------------------------------------ */

/* Diagnostics driven from the shell (spike-b). Without it, as in the WROOM
 * enabler build, they and their 10 KB thread stack are left out.
 */
#if defined(CONFIG_SHELL)

K_THREAD_STACK_DEFINE(reenroll_stack, 10240);
static struct k_thread reenroll_thread;

/* Log an HTTP result safely: deferred logging would read resp after the next
 * request overwrote it, and a base64 body has newlines.
 */
static void log_result(const char *what, int ret)
{
	char snippet[81];
	size_t n = 0;

	for (size_t i = 0; i < resp_len && n < sizeof(snippet) - 1; i++) {
		char ch = resp[i];

		snippet[n++] = (ch == '\r' || ch == '\n') ? ' ' : ch;
	}
	snippet[n] = '\0';
	LOG_INF("REENROLL %s: ret=%d HTTP %u, %zu B", what, ret, resp_status,
		resp_len);
	LOG_INF("REENROLL %s body: %s", what, snippet);
	k_msleep(200);
}

static void reenroll_fn(void *a, void *b, void *c)
{
	static char bearer[1100];
	const char *jwt = spike_mqtt_jwt();
	int ret;

	if (make_csr()) {
		return;
	}
	if (jwt && jwt[0]) {
		snprintk(bearer, sizeof(bearer), "Authorization: Bearer %s\r\n", jwt);
		LOG_INF("REENROLL trying Bearer JWT (%zu-byte header), no client cert",
			strlen(bearer));
		ret = https_post("/.well-known/est/simplereenroll", bearer, false,
				 csr_body);
		log_result("Bearer JWT, no client cert", ret);
		if (ret == 0 && resp_status == 200) {
			size_t len;
			static uint8_t der[1024];

			if (unwrap_pkcs7(resp, der, sizeof(der), &len) == 0) {
				LOG_INF("REENROLL Bearer: new certificate %zu B DER", len);
			}
		}
	} else {
		LOG_WRN("REENROLL: no JWT yet, skipped the Bearer attempt");
	}
	LOG_INF("REENROLL trying mTLS only");
	ret = https_post("/.well-known/est/simplereenroll", NULL, true, csr_body);
	log_result("mTLS only", ret);
}

static int cmd_reenroll(const struct shell *sh, size_t argc, char **argv)
{
	k_thread_create(&reenroll_thread, reenroll_stack,
			K_THREAD_STACK_SIZEOF(reenroll_stack), reenroll_fn, NULL,
			NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	shell_print(sh, "re-enrollment test started");
	return 0;
}

static void csr_fn(void *a, void *b, void *c)
{
	make_csr();
}

static int cmd_csr(const struct shell *sh, size_t argc, char **argv)
{
	k_thread_create(&reenroll_thread, reenroll_stack,
			K_THREAD_STACK_SIZEOF(reenroll_stack), csr_fn, NULL, NULL,
			NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	return 0;
}

/* Time P-256 key generation and one signature, with a throwaway key. */
static void bench_fn(void *a, void *b, void *c)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	uint8_t hash[32] = {1}, sig[64];
	size_t sig_len;
	psa_key_id_t id;
	int64_t t0 = k_uptime_get(), t1;

	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
		LOG_ERR("bench: key generation failed");
		return;
	}
	t1 = k_uptime_get();
	psa_sign_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig,
		      sizeof(sig), &sig_len);
	LOG_INF("MEAS bench: P-256 key generation %lld ms, ECDSA sign %lld ms",
		t1 - t0, k_uptime_get() - t1);
	psa_destroy_key(id);
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	k_thread_create(&reenroll_thread, reenroll_stack,
			K_THREAD_STACK_SIZEOF(reenroll_stack), bench_fn, NULL, NULL,
			NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "external ID %s, certificate %zu B, key %s", device_id,
		    cert_der_len, key_der ? "exported to TLS" : "in PSA only");
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	settings_delete("spike/enroll/cert");
	settings_delete("spike/enroll/otp");
	psa_destroy_key(KEY_ID);
	shell_print(sh, "certificate, one-time password and key deleted; reboot");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_enroll,
	SHELL_CMD(status, NULL, "enrollment state", cmd_status),
	SHELL_CMD(csr, NULL, "log a CSR made with the stored key", cmd_csr),
	SHELL_CMD(bench, NULL, "time P-256 key generation and signing", cmd_bench),
	SHELL_CMD(reenroll, NULL, "task 5.6: simplereenroll tests", cmd_reenroll),
	SHELL_CMD(reset, NULL, "forget certificate, OTP and key", cmd_reset),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(enroll, &sub_enroll, "Spike C: Cumulocity CA enrollment", NULL);

#endif /* CONFIG_SHELL */
