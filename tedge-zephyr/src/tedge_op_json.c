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
