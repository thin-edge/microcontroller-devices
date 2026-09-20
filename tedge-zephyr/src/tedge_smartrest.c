/* SPDX-License-Identifier: Apache-2.0
 *
 * SmartREST 2.0 CSV: reading fields out of a downstream line and quoting a
 * value for an upstream one. No I/O, so it can be unit-tested on native_sim.
 *
 * Cumulocity's CSV: fields are separated by commas; a field may be wrapped in
 * double quotes, and a doubled quote inside such a field means one quote.
 */

#include "tedge_internal.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>

int tedge_sr_field(const char *line, int index, char *out, size_t len)
{
	const char *p = line;
	int field = 0;
	size_t n = 0;

	if (line == NULL || out == NULL || len == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	while (*p != '\0') {
		bool quoted = (*p == '"');

		if (quoted) {
			p++;
		}
		n = 0;
		while (*p != '\0') {
			if (quoted && *p == '"') {
				if (p[1] == '"') { /* escaped quote */
					p += 2;
					if (field == index && n < len - 1) {
						out[n++] = '"';
					}
					continue;
				}
				p++; /* closing quote */
				break;
			}
			if (!quoted && *p == ',') {
				break;
			}
			if (field == index && n < len - 1) {
				out[n++] = *p;
			}
			p++;
		}
		if (field == index) {
			out[n] = '\0';
			return (int)n;
		}
		field++;
		if (*p == ',') {
			p++;
		} else if (*p == '\0') {
			break;
		} else {
			p++; /* stray character after a closing quote */
		}
	}
	return -ENOENT;
}

int tedge_sr_template(const char *line)
{
	char first[8];

	if (tedge_sr_field(line, 0, first, sizeof(first)) < 0) {
		return -EINVAL;
	}
	if (first[0] < '0' || first[0] > '9') {
		return -EINVAL;
	}
	return (int)strtol(first, NULL, 10);
}

int tedge_sr_quote(const char *in, char *out, size_t len)
{
	size_t n = 0;

	if (in == NULL || out == NULL || len < 3) {
		return -EINVAL;
	}
	out[n++] = '"';
	for (const char *p = in; *p != '\0'; p++) {
		size_t need = (*p == '"') ? 2 : 1;

		if (n + need + 2 > len) { /* closing quote + NUL */
			break;
		}
		if (*p == '"') {
			out[n++] = '"';
		}
		/* A newline would end the SmartREST line. */
		out[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
	}
	out[n++] = '"';
	out[n] = '\0';
	return (int)n;
}
