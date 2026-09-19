/* SPDX-License-Identifier: Apache-2.0 */

#include "prov_identity.h"

#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>

#define IDENTITY_KEY "prov/ident"

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct prov_identity *out = param;

	if (key != NULL || len != sizeof(*out)) {
		return 0;
	}
	if (read_cb(cb_arg, out, sizeof(*out)) == sizeof(*out)) {
		return 1; /* found: stop */
	}
	return 0;
}

int prov_identity_load(struct prov_identity *out)
{
	memset(out, 0, sizeof(*out));
	if (settings_subsys_init() != 0) {
		return -ENOENT;
	}
	(void)settings_load_subtree_direct(IDENTITY_KEY, load_cb, out);
	if (out->hostname[0] == '\0') {
		return -ENOENT;
	}
	out->hostname[sizeof(out->hostname) - 1] = '\0';
	out->service[sizeof(out->service) - 1] = '\0';
	return 0;
}

int prov_identity_store(const struct prov_identity *id)
{
	struct prov_identity cur;

	if (prov_identity_load(&cur) == 0 && memcmp(&cur, id, sizeof(cur)) == 0) {
		return 0; /* unchanged: no flash write */
	}
	return settings_save_one(IDENTITY_KEY, id, sizeof(*id));
}
