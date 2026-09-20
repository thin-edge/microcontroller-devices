/* SPDX-License-Identifier: Apache-2.0
 *
 * Just enough JSON to take fields back out of the messages this module
 * built itself: a string field, and an iterator over numeric members. It is
 * not a parser for arbitrary input, and it says so — the only JSON it reads
 * is JSON it wrote, which is why a few hundred bytes are enough where a
 * real parser would cost kilobytes.
 *
 * Pure, and unit-tested on native_sim.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Find "<key>": in @p json and return what follows, or NULL. */
static const char *value_of(const char *json, const char *key)
{
	char pattern[48];
	const char *p;

	if (snprintf(pattern, sizeof(pattern), "\"%s\":", key) >=
	    (int)sizeof(pattern)) {
		return NULL;
	}
	p = strstr(json, pattern);
	return (p != NULL) ? p + strlen(pattern) : NULL;
}

int tedge_json_field(const char *json, const char *key, char *out, size_t len)
{
	const char *p = value_of(json, key);
	size_t n = 0;

	if (out == NULL || len == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	if (p == NULL || *p != '"') {
		return -ENOENT;
	}
	p++;
	while (*p != '\0' && *p != '"' && n < len - 1) {
		if (*p == '\\' && p[1] != '\0') {
			p++; /* keep the escaped character itself */
		}
		out[n++] = *p++;
	}
	out[n] = '\0';
	return (int)n;
}

/* Like tedge_json_field(), but the value may be a number, true or false as
 * well as a string: an operation carries a port and a line count beside its
 * text. The search starts at @p json, so a caller that wants a field of one
 * fragment passes a pointer to that fragment rather than to the document. */
int tedge_json_value(const char *json, const char *key, char *out, size_t len)
{
	const char *p = value_of(json, key);
	size_t n = 0;

	if (out == NULL || len == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	if (p == NULL) {
		return -ENOENT;
	}
	while (*p == ' ') {
		p++;
	}
	if (*p == '"') {
		return tedge_json_field(json, key, out, len);
	}
	/* A bare token ends at the comma or brace that follows it. */
	while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
	       *p != ' ' && n < len - 1) {
		out[n++] = *p++;
	}
	out[n] = '\0';
	return (int)n;
}

int tedge_json_next_number(const char *json, size_t *pos, char *key,
			   size_t key_len, char *value, size_t value_len)
{
	const char *p = json + *pos;

	while (*p != '\0') {
		const char *name, *name_end;
		size_t n = 0;

		if (*p != '"') {
			p++;
			continue;
		}
		name = ++p;
		name_end = strchr(name, '"');
		if (name_end == NULL) {
			break;
		}
		p = name_end + 1;
		if (*p != ':') {
			continue;
		}
		p++;
		if (*p == '"' || *p == '{' || *p == '[') {
			continue; /* not a number: skip this member */
		}
		/* A number: digits, sign, decimal point. */
		while (*p != '\0' && n < value_len - 1 &&
		       (*p == '-' || *p == '+' || *p == '.' ||
			(*p >= '0' && *p <= '9'))) {
			value[n++] = *p++;
		}
		value[n] = '\0';
		if (n == 0) {
			continue;
		}
		snprintf(key, key_len, "%.*s", (int)(name_end - name), name);
		*pos = (size_t)(p - json);
		return 1;
	}
	*pos = strlen(json);
	return 0;
}

/* Copy the string literal starting at the opening quote @p p into @p out,
 * and return the character after the closing quote. */
static const char *copy_string(const char *p, char *out, size_t len)
{
	size_t n = 0;

	p++; /* the opening quote */
	while (*p != '\0' && *p != '"') {
		if (*p == '\\' && p[1] != '\0') {
			p++; /* keep the escaped character itself */
		}
		if (n < len - 1) {
			out[n++] = *p;
		}
		p++;
	}
	out[n] = '\0';
	return (*p == '"') ? p + 1 : p;
}

/* Skip the object or array starting at @p p, quotes and all. */
static const char *skip_nested(const char *p)
{
	int depth = 0;

	do {
		if (*p == '"') {
			char discard[2];

			p = copy_string(p, discard, sizeof(discard));
			continue;
		}
		if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			depth--;
		}
		p++;
	} while (*p != '\0' && depth > 0);
	return p;
}

int tedge_json_next_member(const char *json, size_t *pos, char *key,
			   size_t key_len, char *value, size_t value_len)
{
	const char *p = json + *pos;

	if (key_len == 0 || value_len < 2) {
		return -EINVAL;
	}
	while (*p != '\0') {
		size_t n = 0;

		if (*p != '"') {
			p++;
			continue;
		}
		p = copy_string(p, key, key_len);
		while (*p == ' ') {
			p++;
		}
		if (*p != ':') {
			continue; /* a string that was not a member name */
		}
		p++;
		while (*p == ' ') {
			p++;
		}
		if (*p == '"') {
			p = copy_string(p, value, value_len);
			*pos = (size_t)(p - json);
			return 1;
		}
		if (*p == '{' || *p == '[') {
			/* Report it as itself: a parameter is one value, so
			 * the caller's job is to refuse this, not read it. */
			value[0] = *p;
			value[1] = '\0';
			p = skip_nested(p);
			*pos = (size_t)(p - json);
			return 1;
		}
		/* A bare token ends at the comma or brace that follows it. */
		while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
		       *p != ' ' && n < value_len - 1) {
			value[n++] = *p++;
		}
		value[n] = '\0';
		*pos = (size_t)(p - json);
		return (n > 0) ? 1 : 0;
	}
	*pos = strlen(json);
	return 0;
}
