/* SPDX-License-Identifier: Apache-2.0
 *
 * Turning the JSON of a Cumulocity operation into the comma-separated line
 * the feature handlers already parse.
 *
 * Operations arrive as JSON on devicecontrol/notifications because that is
 * the only delivery that carries the operation's id, and the id is what
 * lets the client report a status for the operation it was given rather
 * than for whichever one the cloud considers oldest. The line keeps the
 * shape of the SmartREST templates it replaces, with the id where the
 * device serial used to be, so nothing downstream had to change.
 *
 * Pure, and unit-tested on native_sim.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

/* Cumulocity delivers an operation as JSON on devicecontrol/notifications,
 * which is the only delivery that carries the operation's id. The fields
 * this client needs are few, so the JSON becomes the same comma-separated
 * line the static templates used to deliver, with the id where the device
 * serial used to be: every handler below keeps working unchanged, and only
 * the status messages have to know about ids.
 *
 * The payload is never logged. A remote-access operation carries a
 * connection key, and a log line is the last place for it.
 */
static bool fragment_at(const char *json, const char *name, const char **at)
{
	char pattern[40];

	snprintf(pattern, sizeof(pattern), "\"%s\"", name);
	*at = strstr(json, pattern);
	return *at != NULL;
}

/* Appends ",<value>" to @p line, quoted as SmartREST needs. */
static void put_field(char *line, size_t len, const char *frag,
		      const char *key)
{
	char raw[200];
	char quoted[224];
	size_t n = strlen(line);

	raw[0] = '\0';
	if (key != NULL) {
		(void)tedge_json_value(frag, key, raw, sizeof(raw));
	}
	(void)tedge_sr_quote(raw, quoted, sizeof(quoted));
	snprintf(line + n, len - n, ",%s", quoted);
}

#if defined(CONFIG_TEDGE_PARAMETERS)
/* A parameter change is shaped unlike every other operation, in two ways.
 *
 * It does not name a fixed fragment: the set's name is the suffix of
 * "c8y_ParameterUpdate_<set>", which is how one device can offer more than
 * one set. And that fragment is **empty** — a marker that says which set is
 * being changed, nothing more. The values arrive in a separate top-level
 * fragment named after the set, and they are the *whole* set rather than
 * only what an operator touched:
 *
 *   {"id":"214989","deviceId":"79211726",
 *    "description":"Update parameter 'tedge'",
 *    "tedge":{"log_level":"dbg","health_interval_s":900,
 *             "required_interval_min":30},
 *    "c8y_ParameterUpdate":{}, "c8y_ParameterUpdate_tedge":{}}
 *
 * (Captured from a real operation; see the openspec findings note. The
 * thin-edge.io plugin reads it the same way, `jq ".operation.\"$TYPE\""`.)
 *
 * An operation built by hand through the REST API more naturally carries
 * the values inside the suffixed fragment, so that is accepted too, as a
 * fallback, when the named fragment is missing or empty.
 *
 * Parameters are also the only operation whose payload is an object rather
 * than a handful of scalars, so the object travels down the line whole,
 * quoted as one SmartREST field, and the handler reads it back out with
 * tedge_sr_field().
 */
#define PARAM_PREFIX "\"c8y_ParameterUpdate_"

/* The object has to survive being quoted into a 768-byte operation line
 * alongside the id and the set's name, and every quote in it doubles. A
 * change of a handful of values is far inside this; a bigger one is
 * refused rather than truncated, because half a change is worse than
 * none. */
#define OP_JSON_MAX 320

static bool parameter_update(const char *json, const char *id, char *line,
			     size_t len)
{
	const char *at = strstr(json, PARAM_PREFIX);
	const char *name, *name_end, *body;
	char set[40];
	char quoted[2 * OP_JSON_MAX + 8];
	int depth = 0;
	size_t n = 0;

	if (at == NULL) {
		return false;
	}
	name = at + strlen(PARAM_PREFIX);
	name_end = strchr(name, '"');
	if (name_end == NULL || (size_t)(name_end - name) >= sizeof(set)) {
		return false;
	}
	snprintf(set, sizeof(set), "%.*s", (int)(name_end - name), name);

	/* The values: the top-level fragment named after the set. Searching
	 * for "<set>": cannot match the marker, whose name ends
	 * ..._<set>" — the character before the name is an underscore there,
	 * not the opening quote. */
	{
		char needle[sizeof(set) + 4];

		snprintf(needle, sizeof(needle), "\"%s\":", set);
		body = strstr(json, needle);
		if (body != NULL) {
			body = strchr(body + strlen(needle) - 1, '{');
		}
	}
	/* Nothing named after the set, or it was empty: fall back to the
	 * marker fragment, which is where an operation built by hand through
	 * the REST API puts them. */
	if (body == NULL || body[1] == '}') {
		const char *marker = strchr(name_end, '{');

		if (marker != NULL && marker[1] != '}') {
			body = marker;
		} else if (body == NULL) {
			body = marker;
		}
	}
	if (body == NULL) {
		return false;
	}
	{
		static char object[OP_JSON_MAX];
		bool in_string = false;

		for (const char *p = body; *p != '\0' && n < sizeof(object) - 1;
		     p++) {
			if (in_string) {
				if (*p == '\\' && p[1] != '\0') {
					object[n++] = *p++;
				} else if (*p == '"') {
					in_string = false;
				}
			} else if (*p == '"') {
				in_string = true;
			} else if (*p == '{') {
				depth++;
			} else if (*p == '}') {
				depth--;
			}
			object[n++] = *p;
			if (depth == 0 && !in_string) {
				break;
			}
		}
		object[n] = '\0';
		if (depth != 0) {
			LOG_WRN("parameter change for '%s' is too big to read",
				set);
			return false;
		}
		(void)tedge_sr_quote(object, quoted, sizeof(quoted));
	}
	snprintf(line, len, "532,%s,%s,%s", id, set, quoted);
	LOG_INF("operation c8y_ParameterUpdate_%s (%s)", set, id);
	return true;
}
#endif /* CONFIG_TEDGE_PARAMETERS */

bool tedge_operation_from_json(const char *json, char *line, size_t len)
{
	static const struct {
		const char *fragment;
		int template_id;
		const char *fields[6];
	} known[] = {
		{ "c8y_Restart", 510, { NULL } },
		{ "c8y_Command", 511, { "text", NULL } },
		{ "c8y_Firmware", 515, { "name", "version", "url", NULL } },
		{ "c8y_LogfileRequest", 522,
		  { "logFile", "dateFrom", "dateTo", "searchText",
		    "maximumLines", NULL } },
		{ "c8y_RemoteAccessConnect", 530,
		  { "hostname", "port", "connectionKey", NULL } },
	};
	char id[24];
	const char *frag;

	if (tedge_json_value(json, "id", id, sizeof(id)) <= 0) {
		return false; /* not an operation, or not one we can answer */
	}
#if defined(CONFIG_TEDGE_PARAMETERS)
	if (parameter_update(json, id, line, len)) {
		return true;
	}
#endif
	for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
		if (!fragment_at(json, known[i].fragment, &frag)) {
			continue;
		}
		snprintf(line, len, "%d,%s", known[i].template_id, id);
		for (int f = 0; known[i].fields[f] != NULL; f++) {
			put_field(line, len, frag, known[i].fields[f]);
		}
		LOG_INF("operation %s (%s)", known[i].fragment, id);
		return true;
	}
	/* An operation this image has no idea about still has to be answered,
	 * so it goes through the dispatcher as an unknown template. */
	snprintf(line, len, "599,%s", id);
	LOG_WRN("an operation arrived that this image does not know (%s)", id);
	return true;
}
