/* SPDX-License-Identifier: Apache-2.0 */

#include "ztp_apply.h"
#include "ztp_crypto.h"
#include "ztp_manifest.h"

#include "net.h"
#include "prov_c8y.h"

#include <errno.h>
#include <string.h>

#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ztp_apply, CONFIG_LOG_DEFAULT_LEVEL);

/* One module's payload, clear or unsealed. wifi.v2 and c8y.v2 INI bodies are a
 * few hundred bytes; 1 KB leaves room for several networks. */
#define PAYLOAD_MAX 1024
#define CT_MAX      (PAYLOAD_MAX + ZTP_SEAL_TAG_LEN)

/* ------------------------------------------------------------------------ */
/* INI                                                                       */
/* ------------------------------------------------------------------------ */

/* Calls @p fn for every key=value line, with the section it is in. The INI
 * bodies come from pkg/server/payload/ini.go: "[section]" headers, then
 * "key=value" lines, no quoting or comments. */
typedef int (*ini_fn)(struct ztp_slice section, bool new_section,
		      struct ztp_kv *kv, void *arg);

static int ini_walk(const char *buf, size_t len, ini_fn fn, void *arg)
{
	const char *cur = buf;
	const char *end = buf + len;
	struct ztp_slice section = { "", 0 };

	while (cur < end) {
		const char *nl = memchr(cur, '\n', end - cur);
		const char *stop = nl ? nl : end;
		const char *line = cur;

		cur = nl ? nl + 1 : end;
		if (stop > line && stop[-1] == '\r') {
			stop--;
		}
		if (stop > line && line[0] == '[' && stop[-1] == ']') {
			section = (struct ztp_slice){ line + 1, stop - line - 2 };
			int rc = fn(section, true, NULL, arg);

			if (rc) {
				return rc;
			}
			continue;
		}
		const char *eq = memchr(line, '=', stop - line);

		if (eq == NULL) {
			continue;
		}
		struct ztp_kv kv = {
			.key = { line, eq - line },
			.val = { eq + 1, stop - eq - 1 },
		};
		int rc = fn(section, false, &kv, arg);

		if (rc) {
			return rc;
		}
	}
	return 0;
}

/* Copy a value into a fixed buffer, refusing to truncate: a truncated SSID,
 * password or token is a wrong one, not a shorter one. */
static int copy_val(char *dst, size_t size, struct ztp_slice v)
{
	if (v.len >= size) {
		return -E2BIG;
	}
	memcpy(dst, v.p, v.len);
	dst[v.len] = '\0';
	return 0;
}

/* ------------------------------------------------------------------------ */
/* wifi.v2                                                                   */
/* ------------------------------------------------------------------------ */

struct wifi_candidate {
	struct app_wifi_creds creds;
	int priority;
	bool open;       /* key_mgmt=NONE */
	bool seen_ssid;
};

static struct {
	struct app_wifi_creds best;
	int best_priority;
	bool have;
	struct wifi_candidate cur;
	bool in_network;
} wifi;

static void wifi_reset(void)
{
	memset(&wifi, 0, sizeof(wifi));
}

/* Close the current [network] section: keep it if it beats the best so far.
 * The first network wins a priority tie, matching declaration order. */
static void wifi_close_section(void)
{
	struct wifi_candidate *c = &wifi.cur;

	if (wifi.in_network && c->seen_ssid &&
	    (!wifi.have || c->priority > wifi.best_priority)) {
		if (c->open) {
			c->creds.psk[0] = '\0';
			c->creds.psk_len = 0;
		}
		wifi.best = c->creds;
		wifi.best_priority = c->priority;
		wifi.have = true;
	}
	memset(c, 0, sizeof(*c));
	wifi.in_network = false;
}

static int wifi_ini(struct ztp_slice section, bool new_section,
		    struct ztp_kv *kv, void *arg)
{
	struct wifi_candidate *c = &wifi.cur;

	ARG_UNUSED(arg);
	if (new_section) {
		wifi_close_section();
		wifi.in_network = ztp_slice_eq(section, "network");
		return 0;
	}
	if (!wifi.in_network) {
		return 0;
	}
	if (ztp_slice_eq(kv->key, "ssid")) {
		if (kv->val.len == 0 || copy_val(c->creds.ssid,
						 sizeof(c->creds.ssid), kv->val)) {
			return -EINVAL;
		}
		c->creds.ssid_len = kv->val.len;
		c->seen_ssid = true;
	} else if (ztp_slice_eq(kv->key, "password")) {
		if (copy_val(c->creds.psk, sizeof(c->creds.psk), kv->val)) {
			return -EINVAL;
		}
		c->creds.psk_len = kv->val.len;
	} else if (ztp_slice_eq(kv->key, "key_mgmt")) {
		c->open = ztp_slice_eq(kv->val, "NONE");
	} else if (ztp_slice_eq(kv->key, "priority")) {
		int p = 0;
		bool neg = kv->val.len && kv->val.p[0] == '-';

		for (size_t i = neg; i < kv->val.len && i < 6; i++) {
			if (kv->val.p[i] < '0' || kv->val.p[i] > '9') {
				return -EINVAL;
			}
			p = p * 10 + (kv->val.p[i] - '0');
		}
		c->priority = neg ? -p : p;
	}
	/* hidden: joining by SSID works for hidden networks as well. */
	return 0;
}

static int wifi_stage(const uint8_t *p, size_t len)
{
	int rc;

	wifi_reset();
	rc = ini_walk((const char *)p, len, wifi_ini, NULL);
	wifi_close_section();
	if (rc) {
		return rc;
	}
	if (!wifi.have) {
		return -ENODATA;
	}
	LOG_INF("wifi.v2: network \"%s\"%s", wifi.best.ssid,
		wifi.best.psk_len ? "" : " (open)");
	return 0;
}

/* Test the credentials by joining, then store them — the same rule Improv
 * follows, so a wrong password never replaces working credentials. */
static int wifi_commit(void)
{
	int rc;

	LOG_INF("Testing credentials for SSID \"%s\"", wifi.best.ssid);
	rc = app_net_try_credentials(
		&wifi.best, K_SECONDS(CONFIG_APP_WIFI_PROV_CONNECT_TIMEOUT_S));
	if (rc != 0) {
		LOG_WRN("Could not join \"%s\" (%d); nothing stored",
			wifi.best.ssid, rc);
		return -ENETUNREACH;
	}
	(void)wifi_credentials_delete_all();
	rc = wifi_credentials_set_personal(
		wifi.best.ssid, wifi.best.ssid_len,
		wifi.best.psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		NULL, 0, wifi.best.psk, wifi.best.psk_len, 0, 0, 0);
	if (rc != 0) {
		LOG_ERR("Storing the credentials failed (%d)", rc);
	}
	return rc;
}

/* ------------------------------------------------------------------------ */
/* c8y.v2                                                                    */
/* ------------------------------------------------------------------------ */

static struct prov_c8y c8y;

static void c8y_reset(void)
{
	memset(&c8y, 0, sizeof(c8y)); /* also scrubs the one-time password */
}

static int c8y_ini(struct ztp_slice section, bool new_section,
		   struct ztp_kv *kv, void *arg)
{
	ARG_UNUSED(arg);
	if (new_section || !ztp_slice_eq(section, "c8y")) {
		return 0;
	}
	if (ztp_slice_eq(kv->key, "url")) {
		return copy_val(c8y.url, sizeof(c8y.url), kv->val);
	}
	if (ztp_slice_eq(kv->key, "tenant")) {
		return copy_val(c8y.tenant, sizeof(c8y.tenant), kv->val);
	}
	if (ztp_slice_eq(kv->key, "external_id")) {
		return copy_val(c8y.external_id, sizeof(c8y.external_id), kv->val);
	}
	if (ztp_slice_eq(kv->key, "one_time_password")) {
		return copy_val(c8y.otp, sizeof(c8y.otp), kv->val);
	}
	return 0;
}

static int c8y_stage(const uint8_t *p, size_t len)
{
	int rc;

	c8y_reset();
	rc = ini_walk((const char *)p, len, c8y_ini, NULL);
	if (rc) {
		c8y_reset();
		return rc;
	}
	if (c8y.url[0] == '\0') {
		return -ENODATA;
	}
	/* Never log the one-time password, only whether there is one. */
	LOG_INF("c8y.v2: %s, external ID \"%s\", %s", c8y.url,
		c8y.external_id[0] ? c8y.external_id : "(device default)",
		c8y.otp[0] ? "one-time password issued"
			   : "no one-time password (device generates one)");
	return 0;
}

static int c8y_commit(void)
{
	int rc = prov_c8y_store(&c8y);

	if (rc) {
		LOG_ERR("Storing the Cumulocity settings failed (%d)", rc);
	}
	c8y_reset();
	return rc;
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------ */

struct ztp_applier {
	const char *type;
	int (*stage)(const uint8_t *payload, size_t len);
	int (*commit)(void);
	void (*reset)(void);
};

/* Commit order is table order: Wi-Fi must be proven before anything that
 * depends on the device being online is kept. */
static const struct ztp_applier appliers[] = {
	{ "wifi.v2", wifi_stage, wifi_commit, wifi_reset },
	{ "c8y.v2", c8y_stage, c8y_commit, c8y_reset },
};

/* __noinit: see the buffer note in ztp.c. */
static __noinit uint8_t payload[PAYLOAD_MAX];
static __noinit uint8_t ct[CT_MAX];

static int b64_exact(struct ztp_slice s, uint8_t *dst, size_t want)
{
	size_t olen;

	if (base64_decode(dst, want, &olen, (const uint8_t *)s.p, s.len) != 0 ||
	    olen != want) {
		return -EPROTO;
	}
	return 0;
}

/* Decode (and for a sealed module, open) a module into `payload`. */
static int module_payload(const struct ztp_module *m, size_t *len)
{
	size_t olen;

	if (!m->sealed) {
		if (base64_decode(payload, sizeof(payload), &olen,
				  (const uint8_t *)m->payload.p,
				  m->payload.len) != 0) {
			return -EPROTO;
		}
		*len = olen;
		return 0;
	}

	uint8_t eph[ZTP_P256_POINT_LEN];
	uint8_t nonce[ZTP_SEAL_NONCE_LEN];
	int rc;

	if (!ztp_slice_eq(m->format, "raw")) {
		return -ENOTSUP; /* both appliers take INI bodies */
	}
	if (b64_exact(m->eph, eph, sizeof(eph)) ||
	    b64_exact(m->nonce, nonce, sizeof(nonce)) ||
	    base64_decode(ct, sizeof(ct), &olen, (const uint8_t *)m->ciphertext.p,
			  m->ciphertext.len) != 0) {
		return -EPROTO;
	}
	rc = ztp_open(eph, nonce, ct, olen, payload, sizeof(payload), len);
	memset(ct, 0, sizeof(ct));
	return rc;
}

static const struct ztp_applier *find_applier(struct ztp_slice type)
{
	for (size_t i = 0; i < ARRAY_SIZE(appliers); i++) {
		if (ztp_slice_eq(type, appliers[i].type)) {
			return &appliers[i];
		}
	}
	return NULL;
}

static void reset_all(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(appliers); i++) {
		appliers[i].reset();
	}
	memset(payload, 0, sizeof(payload));
}

int ztp_apply_manifest(const char *manifest, size_t len, const char *device_id)
{
	struct ztp_manifest_hdr hdr;
	struct ztp_module m;
	const char *cur = manifest;
	const char *end = manifest + len;
	bool staged[ARRAY_SIZE(appliers)] = { false };
	bool any = false;
	int rc;

	ztp_manifest_header(manifest, len, &hdr);
	if (!hdr.version_ok) {
		LOG_ERR("manifest: unsupported protocol version");
		return -EPROTO;
	}
	if (!ztp_slice_eq(hdr.device_id, device_id)) {
		/* A clear module (Wi-Fi) for another device must not be
		 * applied here; sealed ones would fail to open anyway. */
		LOG_ERR("manifest is for \"%.*s\", not \"%s\"",
			(int)hdr.device_id.len, hdr.device_id.p, device_id);
		return -EPROTO;
	}

	/* Phase 1: stage every module. Nothing is written yet. */
	reset_all();
	while ((rc = ztp_manifest_next_module(&cur, end, &m)) == 1) {
		const struct ztp_applier *a = find_applier(m.type);
		size_t plen;

		if (a == NULL) {
			LOG_INF("module %.*s: skipped (no applier)",
				(int)m.type.len, m.type.p);
			continue;
		}
		rc = module_payload(&m, &plen);
		if (rc == 0) {
			rc = a->stage(payload, plen);
		}
		memset(payload, 0, sizeof(payload));
		if (rc) {
			LOG_ERR("module %s: %s (%d)", a->type,
				rc == -EBADMSG ? "sealed payload did not open"
					       : "invalid",
				rc);
			reset_all();
			return rc;
		}
		staged[a - appliers] = true;
		any = true;
	}
	if (rc < 0) {
		LOG_ERR("manifest: malformed module line");
		reset_all();
		return rc;
	}
	if (!any) {
		LOG_ERR("bundle carries no module this device can apply");
		return -ENODATA;
	}

	/* Phase 2: commit in table order; stop at the first failure. */
	for (size_t i = 0; i < ARRAY_SIZE(appliers); i++) {
		if (!staged[i]) {
			continue;
		}
		rc = appliers[i].commit();
		if (rc) {
			reset_all();
			return rc;
		}
	}
	reset_all();
	return 0;
}
