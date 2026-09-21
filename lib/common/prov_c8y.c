/* SPDX-License-Identifier: Apache-2.0 */

#include "prov_c8y.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define KEY_ROOT "prov/c8y"

struct field {
	const char *name;
	size_t offset;
	size_t size;
};

#define FIELD(n) { #n, offsetof(struct prov_c8y, n), sizeof(((struct prov_c8y *)0)->n) }

static const struct field fields[] = {
	FIELD(url),
	FIELD(tenant),
	FIELD(external_id),
	FIELD(otp),
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct prov_c8y *out = param;

	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		if (strcmp(key, fields[i].name) != 0) {
			continue;
		}
		char *dst = (char *)out + fields[i].offset;

		if (len >= fields[i].size) {
			return 0; /* never truncate a credential: ignore it */
		}
		ssize_t n = read_cb(cb_arg, dst, len);

		dst[n > 0 ? n : 0] = '\0';
		return 0;
	}
	return 0;
}

int prov_c8y_store(const struct prov_c8y *c)
{
	int rc = settings_subsys_init();

	if (rc) {
		return rc;
	}
	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		const char *v = (const char *)c + fields[i].offset;
		char key[sizeof(KEY_ROOT) + 16];

		if (v[0] == '\0') {
			continue;
		}
		snprintk(key, sizeof(key), KEY_ROOT "/%s", fields[i].name);
		rc = settings_save_one(key, v, strlen(v));
		if (rc) {
			return rc;
		}
	}
	return 0;
}

int prov_c8y_load(struct prov_c8y *out)
{
	memset(out, 0, sizeof(*out));
	if (settings_subsys_init() != 0) {
		return -ENOENT;
	}
	(void)settings_load_subtree_direct(KEY_ROOT, load_cb, out);
	return (out->url[0] || out->otp[0]) ? 0 : -ENOENT;
}

void prov_c8y_clear(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		char key[sizeof(KEY_ROOT) + 16];

		if (strcmp(fields[i].name, "external_id") == 0) {
			continue; /* the device's identity from now on */
		}
		snprintk(key, sizeof(key), KEY_ROOT "/%s", fields[i].name);
		(void)settings_delete(key);
	}
}

int prov_c8y_external_id(char *out, size_t size)
{
	struct prov_c8y c;

	(void)prov_c8y_load(&c);
	if (c.external_id[0] == '\0' || strlen(c.external_id) >= size) {
		return -ENOENT;
	}
	strcpy(out, c.external_id);
	return 0;
}

#if defined(CONFIG_TEDGE_TRANSPORT_C8Y)

#include <tedge/tedge.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(prov_c8y, CONFIG_LOG_DEFAULT_LEVEL);

/* The ZTP server sends a URL; the client wants a bare host. Strip a scheme
 * and anything from the first '/' on. */
static void url_to_host(char *url)
{
	char *p = strstr(url, "://");
	char *slash;

	if (p != NULL) {
		memmove(url, p + 3, strlen(p + 3) + 1);
	}
	slash = strchr(url, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
}

int prov_c8y_handoff(void)
{
	struct prov_c8y c;
	int rc = 0;

	if (prov_c8y_load(&c) != 0) {
		return 0; /* nothing from the provisioner */
	}
	if (c.url[0] != '\0') {
		url_to_host(c.url);
		rc = tedge_set_c8y_url(c.url);
		if (rc) {
			LOG_ERR("ZTP tenant \"%s\" not accepted (%d)", c.url, rc);
		} else {
			LOG_INF("Cumulocity tenant from ZTP: %s", c.url);
		}
	}
	if (rc == 0 && c.otp[0] != '\0') {
		rc = tedge_set_enroll_otp(c.otp);
		if (rc) {
			LOG_ERR("ZTP one-time password not accepted (%d)", rc);
		} else {
			/* Never the password itself. */
			LOG_INF("Enrolling as \"%s\" with the one-time password "
				"from ZTP", c.external_id);
		}
	}
	if (rc == 0) {
		prov_c8y_clear();
	}
	memset(&c, 0, sizeof(c));
	return rc;
}

#endif /* CONFIG_TEDGE_TRANSPORT_C8Y */
