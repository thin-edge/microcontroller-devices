/* SPDX-License-Identifier: Apache-2.0
 *
 * The remote-access allow-list: "host:port" entries separated by commas.
 * Pure, and unit-tested on native_sim.
 */

#include "tedge_internal.h"

#include <stdio.h>
#include <string.h>

bool tedge_ra_in_allow_list(const char *list, const char *host, uint16_t port)
{
	char entry[80];

	snprintf(entry, sizeof(entry), "%s:%u", host, port);
	for (const char *p = list; *p != '\0';) {
		const char *comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);

		if (n == strlen(entry) && strncmp(p, entry, n) == 0) {
			return true;
		}
		p += n + (comma ? 1 : 0);
	}
	return false;
}
