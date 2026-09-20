/* SPDX-License-Identifier: Apache-2.0
 *
 * URL handling for downloads: splitting, resolving a redirect target, and
 * deciding whether a host belongs to the tenant (which decides whether the
 * cloud token may be sent to it). Pure, and unit-tested on native_sim.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Split "scheme://host[:port]/path" into its parts. */
int tedge_url_split(const char *url, bool *tls, char *host, size_t host_len,
		     uint16_t *port, const char **path)
{
	const char *p;

	if (strncmp(url, "https://", 8) == 0) {
		*tls = true;
		*port = 443;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		*tls = false;
		*port = 80;
		p = url + 7;
	} else {
		return -EINVAL;
	}

	const char *slash = strchr(p, '/');
	const char *colon = memchr(p, ':', slash ? (size_t)(slash - p)
						 : strlen(p));
	size_t n = colon ? (size_t)(colon - p)
			 : (slash ? (size_t)(slash - p) : strlen(p));

	if (n == 0 || n >= host_len) {
		return -EINVAL;
	}
	memcpy(host, p, n);
	host[n] = '\0';
	if (colon != NULL) {
		*port = (uint16_t)strtoul(colon + 1, NULL, 10);
		if (*port == 0) {
			return -EINVAL;
		}
	}
	*path = slash ? slash : "/";
	return 0;
}

/* True when @p host is the tenant or a host inside its parent domain, which
 * is where Cumulocity serves binaries from (t<id>.<domain>). */
bool tedge_url_is_tenant(const char *host)
{
	const char *tenant = tedge_c8y_host();
	const char *parent = strchr(tenant, '.');
	size_t plen;

	if (host == NULL || tenant[0] == '\0') {
		return false;
	}
	if (strcmp(host, tenant) == 0) {
		return true;
	}
	if (parent == NULL) {
		return false;
	}
	plen = strlen(parent);
	return strlen(host) > plen &&
	       strcmp(host + strlen(host) - plen, parent) == 0;
}

/* Resolve a redirect target against the URL it came from. */
int tedge_url_resolve(const char *base, const char *location, char *out,
		      size_t len)
{
	if (location == NULL || location[0] == '\0') {
		return -EINVAL;
	}
	if (strncmp(location, "http://", 7) == 0 ||
	    strncmp(location, "https://", 8) == 0) {
		return (snprintf(out, len, "%s", location) < (int)len) ? 0
								      : -ENOSPC;
	}
	/* Relative: keep the base's scheme and host. */
	const char *after_scheme = strstr(base, "://");

	if (after_scheme == NULL) {
		return -EINVAL;
	}
	const char *slash = strchr(after_scheme + 3, '/');
	size_t root = slash ? (size_t)(slash - base) : strlen(base);

	if (location[0] == '/') {
		return (snprintf(out, len, "%.*s%s", (int)root, base,
				 location) < (int)len)
			       ? 0
			       : -ENOSPC;
	}
	return (snprintf(out, len, "%.*s/%s", (int)root, base, location) <
		(int)len)
		       ? 0
		       : -ENOSPC;
}
