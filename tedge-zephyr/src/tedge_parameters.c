/* SPDX-License-Identifier: Apache-2.0
 *
 * The settings an operator may change from the cloud.
 *
 * The application declares a named set of parameters as a const table: a
 * name, a type, a default and the limits of what the value may hold. This
 * file keeps the current values, loads whatever was stored over the
 * defaults, validates every change against the declaration before any of it
 * is applied, stores what actually changed, and reports the set as twin
 * state.
 *
 * Three places have to agree, and they are kept in this order: the values
 * here, the settings subtree "tedge/param/<set>/<name>", and the twin
 * fragment named after the set. Nothing is uploaded or downloaded — a
 * change travels inside its operation, which is why this file needs no HTTP
 * client and no filesystem.
 *
 * The declaration is the application's flash. What costs RAM here is one
 * current value per declared parameter (CONFIG_TEDGE_PARAMETERS_MAX), plus
 * one heap allocation per string parameter, sized by the declaration and
 * made once.
 */

#include "tedge_internal.h"
#include <tedge/tedge.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

/* A device declares one set per thing it is; four is already generous, and
 * a set costs a pointer table whether or not it is used. This is not a
 * Kconfig option because nothing about a device's shape suggests a number
 * an integrator would want to tune. */
#define MAX_SETS 4

/* Long enough for the longest name a set and a parameter can have between
 * them, under the settings key prefix. */
#define KEY_MAX 64

struct param_value {
	union {
		bool b;
		int32_t i;
		char *s; /* max_len + 1 bytes, allocated at declaration */
	} v;
};

struct param_set {
	const char *name;
	const struct tedge_parameter *params;
	size_t count;
	size_t base; /* where this set starts in values[] */
	tedge_parameters_changed_t on_change;
	void *user_data;
};

static struct param_set sets[MAX_SETS];
static size_t n_sets;
static struct param_value values[CONFIG_TEDGE_PARAMETERS_MAX];
static size_t n_values;

/* ------------------------------------------------------------------------ */
/* Lookup                                                                    */
/* ------------------------------------------------------------------------ */

static struct param_set *find_set(const char *name)
{
	for (size_t i = 0; i < n_sets; i++) {
		if (strcmp(sets[i].name, name) == 0) {
			return &sets[i];
		}
	}
	return NULL;
}

static const struct tedge_parameter *find_param(const struct param_set *set,
						const char *name, size_t *index)
{
	for (size_t i = 0; i < set->count; i++) {
		if (strcmp(set->params[i].name, name) == 0) {
			*index = i;
			return &set->params[i];
		}
	}
	return NULL;
}

static struct param_value *value_of(const struct param_set *set, size_t index)
{
	return &values[set->base + index];
}

/* ------------------------------------------------------------------------ */
/* Values                                                                    */
/* ------------------------------------------------------------------------ */

static bool enum_allows(const struct tedge_parameter *p, const char *s)
{
	for (const char *const *a = p->allowed; a != NULL && *a != NULL; a++) {
		if (strcmp(*a, s) == 0) {
			return true;
		}
	}
	return false;
}

static size_t string_capacity(const struct tedge_parameter *p)
{
	if (p->type == TEDGE_PARAM_TYPE_STRING) {
		return (size_t)p->max_len + 1;
	}
	/* An enum can only ever hold one of its own values. */
	size_t longest = 0;

	for (const char *const *a = p->allowed; a != NULL && *a != NULL; a++) {
		size_t n = strlen(*a);

		if (n > longest) {
			longest = n;
		}
	}
	return longest + 1;
}

static void set_default(const struct tedge_parameter *p, struct param_value *v)
{
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		v->v.b = p->def.b;
		break;
	case TEDGE_PARAM_TYPE_INT:
		v->v.i = p->def.i;
		break;
	case TEDGE_PARAM_TYPE_STRING:
	case TEDGE_PARAM_TYPE_ENUM:
		if (v->v.s != NULL) {
			snprintf(v->v.s, string_capacity(p), "%s",
				 p->def.s != NULL ? p->def.s : "");
		}
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* Validation                                                                */
/*                                                                           */
/* One value against its declaration. @p reason says what was wrong in the   */
/* words an operator reads in the failed operation, so it always names the   */
/* parameter and the limit it broke.                                         */
/* ------------------------------------------------------------------------ */

static int check_value(const struct tedge_parameter *p, const char *raw,
		       char *reason, size_t reason_len)
{
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		if (strcmp(raw, "true") != 0 && strcmp(raw, "false") != 0) {
			snprintf(reason, reason_len,
				 "%s: expected true or false, got '%s'",
				 p->name, raw);
			return -EINVAL;
		}
		return 0;
	case TEDGE_PARAM_TYPE_INT: {
		char *end;
		long v;

		errno = 0;
		v = strtol(raw, &end, 10);
		if (raw[0] == '\0' || end == raw || *end != '\0' ||
		    errno == ERANGE || v < INT32_MIN || v > INT32_MAX) {
			snprintf(reason, reason_len,
				 "%s: expected a whole number, got '%s'",
				 p->name, raw);
			return -EINVAL;
		}
		if (v < p->min || v > p->max) {
			snprintf(reason, reason_len,
				 "%s: %ld is outside %d..%d", p->name, v,
				 p->min, p->max);
			return -ERANGE;
		}
		return 0;
	}
	case TEDGE_PARAM_TYPE_STRING:
		if (strlen(raw) > p->max_len) {
			snprintf(reason, reason_len,
				 "%s: %zu characters, at most %u allowed",
				 p->name, strlen(raw), p->max_len);
			return -EINVAL;
		}
		return 0;
	case TEDGE_PARAM_TYPE_ENUM:
		if (!enum_allows(p, raw)) {
			size_t n = snprintf(reason, reason_len,
					    "%s: '%s' is not one of", p->name,
					    raw);

			for (const char *const *a = p->allowed;
			     a != NULL && *a != NULL && n < reason_len; a++) {
				n += snprintf(reason + n, reason_len - n, " %s",
					      *a);
			}
			return -EINVAL;
		}
		return 0;
	}
	snprintf(reason, reason_len, "%s: unknown type", p->name);
	return -EINVAL;
}

/* ------------------------------------------------------------------------ */
/* Storage                                                                   */
/*                                                                           */
/* One settings key per parameter, written only when a value actually        */
/* changes, so an operator pressing "save" with nothing changed costs no     */
/* flash.                                                                    */
/* ------------------------------------------------------------------------ */

static void key_of(const char *set, const char *name, char *out, size_t len)
{
	snprintf(out, len, TEDGE_KEY_PARAM "/%s/%s", set, name);
}

static int store(const struct param_set *set, const struct tedge_parameter *p,
		 const struct param_value *v)
{
	char key[KEY_MAX];

	key_of(set->name, p->name, key, sizeof(key));
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		return settings_save_one(key, &v->v.b, sizeof(v->v.b));
	case TEDGE_PARAM_TYPE_INT:
		return settings_save_one(key, &v->v.i, sizeof(v->v.i));
	case TEDGE_PARAM_TYPE_STRING:
	case TEDGE_PARAM_TYPE_ENUM:
		return settings_save_one(key, v->v.s, strlen(v->v.s) + 1);
	}
	return -EINVAL;
}

struct load_ctx {
	const struct tedge_parameter *p;
	struct param_value *v;
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct load_ctx *ctx = param;
	const struct tedge_parameter *p = ctx->p;

	ARG_UNUSED(key);
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		if (len == sizeof(bool)) {
			(void)read_cb(cb_arg, &ctx->v->v.b, sizeof(bool));
		}
		break;
	case TEDGE_PARAM_TYPE_INT:
		if (len == sizeof(int32_t)) {
			(void)read_cb(cb_arg, &ctx->v->v.i, sizeof(int32_t));
		}
		break;
	case TEDGE_PARAM_TYPE_STRING:
	case TEDGE_PARAM_TYPE_ENUM: {
		size_t cap = string_capacity(p);

		if (ctx->v->v.s != NULL && len > 0 && len <= cap) {
			ssize_t n = read_cb(cb_arg, ctx->v->v.s, cap - 1);

			if (n >= 0) {
				ctx->v->v.s[n] = '\0';
			}
		}
		break;
	}
	}
	return 0;
}

/* Put whatever is stored over the defaults. A stored value that no longer
 * fits its declaration — a firmware update narrowed a range — is left
 * behind rather than loaded, so the device runs what it can defend. */
static void load_stored(struct param_set *set)
{
	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];
		struct param_value *v = value_of(set, i);
		struct param_value before = *v;
		struct load_ctx ctx = { .p = p, .v = v };
		char key[KEY_MAX];
		char raw[64];
		char reason[96];

		key_of(set->name, p->name, key, sizeof(key));
		(void)settings_load_subtree_direct(key, load_cb, &ctx);

		/* Re-check it, because the declaration may have moved. */
		switch (p->type) {
		case TEDGE_PARAM_TYPE_BOOL:
			continue;
		case TEDGE_PARAM_TYPE_INT:
			snprintf(raw, sizeof(raw), "%d", v->v.i);
			break;
		case TEDGE_PARAM_TYPE_STRING:
		case TEDGE_PARAM_TYPE_ENUM:
			if (v->v.s == NULL) {
				continue;
			}
			snprintf(raw, sizeof(raw), "%s", v->v.s);
			break;
		}
		if (check_value(p, raw, reason, sizeof(reason)) != 0) {
			LOG_WRN("params: %s/%s: stored value dropped (%s)",
				set->name, p->name, reason);
			*v = before;
			(void)settings_delete(key);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* The twin                                                                  */
/*                                                                           */
/* The set goes out as one fragment named after it. tedge_publish_twin()     */
/* keeps the value and republishes it after every reconnect, on whichever    */
/* transport is built in, so "on every connect" costs nothing here.          */
/* ------------------------------------------------------------------------ */

static size_t twin_size(const struct param_set *set)
{
	size_t n = 3; /* {} and the NUL */

	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];

		/* "name":value, — a string's escapes can double its length. */
		n += strlen(p->name) + 6;
		n += (p->type == TEDGE_PARAM_TYPE_INT) ? 12
						       : 2 * string_capacity(p) + 2;
	}
	return n;
}

static void publish(const struct param_set *set)
{
	size_t size = twin_size(set);
	char *json = tedge_alloc(size);
	size_t n;

	if (json == NULL) {
		LOG_ERR("params: %s: no heap for the twin fragment", set->name);
		return;
	}
	n = snprintf(json, size, "{");
	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];
		const struct param_value *v = value_of(set, i);

		n += snprintf(json + n, size - n, "%s\"%s\":", i ? "," : "",
			      p->name);
		switch (p->type) {
		case TEDGE_PARAM_TYPE_BOOL:
			n += snprintf(json + n, size - n, "%s",
				      v->v.b ? "true" : "false");
			break;
		case TEDGE_PARAM_TYPE_INT:
			n += snprintf(json + n, size - n, "%d", v->v.i);
			break;
		case TEDGE_PARAM_TYPE_STRING:
		case TEDGE_PARAM_TYPE_ENUM: {
			char *escaped = json + n + 1;
			size_t room = size - n - 3;

			json[n++] = '"';
			tedge_json_escape(v->v.s != NULL ? v->v.s : "", escaped,
					  room);
			n += strlen(escaped);
			n += snprintf(json + n, size - n, "\"");
			break;
		}
		}
	}
	(void)snprintf(json + n, size - n, "}");
	(void)tedge_publish_twin(set->name, json);
	tedge_free(json);
}

/* ------------------------------------------------------------------------ */
/* Declaring                                                                 */
/* ------------------------------------------------------------------------ */

/* A declaration that contradicts itself is a programming error, and one the
 * device can see at startup rather than the first time the cloud touches
 * it. */
static int check_declaration(const struct tedge_parameter *params, size_t count)
{
	char reason[96];

	for (size_t i = 0; i < count; i++) {
		const struct tedge_parameter *p = &params[i];

		if (p->name == NULL || p->name[0] == '\0') {
			return -EINVAL;
		}
		for (size_t j = 0; j < i; j++) {
			if (strcmp(params[j].name, p->name) == 0) {
				LOG_ERR("params: '%s' declared twice", p->name);
				return -EINVAL;
			}
		}
		switch (p->type) {
		case TEDGE_PARAM_TYPE_BOOL:
			break;
		case TEDGE_PARAM_TYPE_INT: {
			char raw[16];

			if (p->min > p->max) {
				LOG_ERR("params: '%s': min above max", p->name);
				return -EINVAL;
			}
			snprintf(raw, sizeof(raw), "%d", p->def.i);
			if (check_value(p, raw, reason, sizeof(reason)) != 0) {
				LOG_ERR("params: default %s", reason);
				return -EINVAL;
			}
			break;
		}
		case TEDGE_PARAM_TYPE_STRING:
			if (p->max_len == 0) {
				LOG_ERR("params: '%s': zero max_len", p->name);
				return -EINVAL;
			}
			if (p->def.s != NULL &&
			    check_value(p, p->def.s, reason, sizeof(reason)) != 0) {
				LOG_ERR("params: default %s", reason);
				return -EINVAL;
			}
			break;
		case TEDGE_PARAM_TYPE_ENUM:
			if (p->allowed == NULL || p->allowed[0] == NULL) {
				LOG_ERR("params: '%s': no allowed values",
					p->name);
				return -EINVAL;
			}
			if (p->def.s == NULL ||
			    check_value(p, p->def.s, reason, sizeof(reason)) != 0) {
				LOG_ERR("params: default %s", reason);
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

int tedge_declare_parameters(const char *set_name,
			     const struct tedge_parameter *params, size_t count,
			     tedge_parameters_changed_t on_change,
			     void *user_data)
{
	struct param_set *set;
	int rc;

	if (set_name == NULL || set_name[0] == '\0' || params == NULL ||
	    count == 0) {
		return -EINVAL;
	}
	if (find_set(set_name) != NULL) {
		return -EEXIST;
	}
	if (n_sets == MAX_SETS) {
		LOG_ERR("params: no free set slot (%d)", MAX_SETS);
		return -ENOSPC;
	}
	if (n_values + count > CONFIG_TEDGE_PARAMETERS_MAX) {
		LOG_ERR("params: %zu parameters wanted, %d left (raise "
			"CONFIG_TEDGE_PARAMETERS_MAX)",
			count, CONFIG_TEDGE_PARAMETERS_MAX - (int)n_values);
		return -ENOSPC;
	}
	if (strlen(TEDGE_KEY_PARAM) + strlen(set_name) + 2 >= KEY_MAX) {
		LOG_ERR("params: set name '%s' is too long", set_name);
		return -EINVAL;
	}
	rc = check_declaration(params, count);
	if (rc != 0) {
		return rc;
	}

	set = &sets[n_sets];
	set->name = set_name;
	set->params = params;
	set->count = count;
	set->base = n_values;
	set->on_change = on_change;
	set->user_data = user_data;

	for (size_t i = 0; i < count; i++) {
		const struct tedge_parameter *p = &params[i];
		struct param_value *v = &values[n_values + i];

		memset(v, 0, sizeof(*v));
		if (p->type == TEDGE_PARAM_TYPE_STRING ||
		    p->type == TEDGE_PARAM_TYPE_ENUM) {
			v->v.s = tedge_alloc(string_capacity(p));
			if (v->v.s == NULL) {
				/* Give back what this call took: a set that is
				 * only half there would report values it could
				 * not hold. */
				for (size_t j = 0; j < i; j++) {
					tedge_free(values[n_values + j].v.s);
					values[n_values + j].v.s = NULL;
				}
				return -ENOMEM;
			}
		}
		set_default(p, v);
	}
	n_values += count;
	n_sets++;

	load_stored(set);
	publish(set);
	LOG_INF("params: declared '%s' with %zu parameters", set_name, count);
	return 0;
}

#if defined(CONFIG_TEDGE_PARAMETERS_SCHEMA)
/* ------------------------------------------------------------------------ */
/* The schema                                                                */
/*                                                                           */
/* What has to be registered in the tenant's Digital Twin Manager before an  */
/* operator can see or change a set. It is generated from the same table     */
/* the validation uses, so the two cannot drift — which is the whole point:  */
/* a schema written by hand beside a C table is a transcription error        */
/* waiting to happen.                                                        */
/*                                                                           */
/* The whole registration body is produced, not a bare schema, because that  */
/* is what the person pasting it into the API call needs. "identifier" is    */
/* the set's name, and "contexts" has to contain both "asset" and            */
/* "operation" or the UI shows the values but refuses to let anyone edit     */
/* them.                                                                     */
/* ------------------------------------------------------------------------ */

/* snprintf() reports what it *would* have written, so n can run past the
 * buffer; every caller below stops as soon as it does. */
#define APPEND(...)                                                            \
	do {                                                                   \
		if (n < len) {                                                 \
			n += snprintf(out + n, len - n, __VA_ARGS__);           \
		} else {                                                       \
			n += snprintf(NULL, 0, __VA_ARGS__);                   \
		}                                                              \
	} while (0)

/* A JSON string, escaped straight into the output one character at a
 * time: descriptions are as long as their author made them, and the only
 * limit is the output buffer, which the caller hears about (n >= len). */
static size_t quoted(char *out, size_t len, size_t n, const char *s)
{
	char in[2] = { 0 };
	char escaped[8];

	APPEND("\"");
	for (; s != NULL && *s != '\0'; s++) {
		in[0] = *s;
		tedge_json_escape(in, escaped, sizeof(escaped));
		APPEND("%s", escaped);
	}
	APPEND("\"");
	return n;
}

int tedge_params_schema(const char *set_name, char *out, size_t len)
{
	const struct param_set *set = find_set(set_name);
	size_t n = 0;

	if (out == NULL || len == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	if (set == NULL) {
		return -ENOENT;
	}

	APPEND("{\"identifier\":");
	n = quoted(out, len, n, set->name);
	APPEND(",\"jsonSchema\":{\"type\":\"object\",\"title\":");
	n = quoted(out, len, n, set->name);
	APPEND(",\"properties\":{");

	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];

		APPEND("%s", i ? "," : "");
		n = quoted(out, len, n, p->name);
		APPEND(":{\"title\":");
		n = quoted(out, len, n, p->name);
		/* "order" is what the UI lays the fields out by, and the
		 * declaration's own order is the one the author meant. */
		APPEND(",\"order\":%zu", i + 1);
		if (p->description != NULL && p->description[0] != '\0') {
			APPEND(",\"description\":");
			n = quoted(out, len, n, p->description);
		}
		switch (p->type) {
		case TEDGE_PARAM_TYPE_BOOL:
			APPEND(",\"type\":\"boolean\",\"default\":%s",
			       p->def.b ? "true" : "false");
			break;
		case TEDGE_PARAM_TYPE_INT:
			APPEND(",\"type\":\"integer\",\"default\":%d,"
			       "\"minimum\":%d,\"maximum\":%d",
			       p->def.i, p->min, p->max);
			break;
		case TEDGE_PARAM_TYPE_STRING:
			APPEND(",\"type\":\"string\",\"maxLength\":%u,"
			       "\"default\":", p->max_len);
			n = quoted(out, len, n, p->def.s);
			break;
		case TEDGE_PARAM_TYPE_ENUM:
			APPEND(",\"type\":\"string\",\"enum\":[");
			for (const char *const *a = p->allowed;
			     a != NULL && *a != NULL; a++) {
				APPEND("%s", a != p->allowed ? "," : "");
				n = quoted(out, len, n, *a);
			}
			APPEND("],\"default\":");
			n = quoted(out, len, n, p->def.s);
			break;
		}
		APPEND("}");
	}
	APPEND("}},\"contexts\":[\"asset\",\"event\",\"operation\"]}");

	if (n >= len) {
		out[0] = '\0';
		return -ENOMEM;
	}
	return (int)n;
}

#undef APPEND

/* ------------------------------------------------------------------------ */
/* "tedge params schema"                                                     */
/*                                                                           */
/* Whoever registers a set in the tenant needs its schema, and the device is */
/* the only thing that knows what the running image actually declares. A     */
/* console command is the shortest path from one to the other.               */
/* ------------------------------------------------------------------------ */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

/* Big enough for a set of a dozen parameters with their descriptions; the
 * generator says so rather than truncating when it is not. */
#define SCHEMA_BUF 2048

static int print_schema(const struct shell *sh, const char *set_name)
{
	char *buf = tedge_alloc(SCHEMA_BUF);
	int rc;

	if (buf == NULL) {
		shell_error(sh, "out of client heap");
		return -ENOMEM;
	}
	rc = tedge_params_schema(set_name, buf, SCHEMA_BUF);
	if (rc == -ENOENT) {
		shell_error(sh, "no parameter set '%s' is declared", set_name);
	} else if (rc == -ENOMEM) {
		shell_error(sh, "'%s' needs more than %d bytes of schema",
			    set_name, SCHEMA_BUF);
	} else if (rc < 0) {
		shell_error(sh, "could not produce the schema (%d)", rc);
	} else {
		shell_print(sh, "%s", buf);
	}
	tedge_free(buf);
	return rc < 0 ? rc : 0;
}

static int cmd_params_schema(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		return print_schema(sh, argv[1]);
	}
	if (n_sets == 0) {
		shell_warn(sh, "this image declares no parameter set");
		return 0;
	}
	for (size_t i = 0; i < n_sets; i++) {
		int rc = print_schema(sh, sets[i].name);

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int cmd_params_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (n_sets == 0) {
		shell_warn(sh, "this image declares no parameter set");
		return 0;
	}
	for (size_t i = 0; i < n_sets; i++) {
		shell_print(sh, "%s (%zu parameters)", sets[i].name,
			    sets[i].count);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	params_cmds,
	SHELL_CMD_ARG(schema, NULL,
		      "Print the registration body for a set, or for every "
		      "set.\nUsage: tedge params schema [<set>]",
		      cmd_params_schema, 1, 1),
	SHELL_CMD(list, NULL, "List the declared parameter sets",
		  cmd_params_list),
	SHELL_SUBCMD_SET_END);

/* The root itself belongs to tedge_core.c; this only adds a branch to it. */
SHELL_SUBCMD_ADD((tedge), params, &params_cmds,
		 "Parameters the cloud may change", NULL, 1, 0);
#endif /* CONFIG_SHELL */

#endif /* CONFIG_TEDGE_PARAMETERS_SCHEMA */

/* ------------------------------------------------------------------------ */
/* Applying a change                                                         */
/*                                                                           */
/* All-or-nothing: every value in the change is checked against its          */
/* declaration before any of it is applied, because a half-applied set is    */
/* how a device ends up in a state nobody can reproduce.                     */
/*                                                                           */
/* The values are put in place, the application is asked, and only then are  */
/* they written to flash. D3 describes the hook as running after they are    */
/* stored; doing it this way round means a refusal costs no flash write at   */
/* all, and the promise the specification makes — that a refused change      */
/* leaves the device running and reporting its previous values — is the      */
/* same either way.                                                          */
/* ------------------------------------------------------------------------ */

/* The current values of one set, kept whole so a refusal can put them back
 * exactly as they were. Strings live in the tail of the same allocation. */
struct backup {
	struct param_value *value;
	char *strings;
	size_t count;
};

static void backup_free(struct backup *b)
{
	tedge_free(b->value);
	b->value = NULL;
	b->strings = NULL;
}

static int backup_take(const struct param_set *set, struct backup *b)
{
	size_t strings = 0;
	size_t offset = 0;
	char *pool;

	for (size_t i = 0; i < set->count; i++) {
		strings += string_capacity(&set->params[i]);
	}
	b->value = tedge_alloc(set->count * sizeof(*b->value) + strings);
	if (b->value == NULL) {
		return -ENOMEM;
	}
	b->count = set->count;
	pool = (char *)(b->value + set->count);
	b->strings = pool;

	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];
		const struct param_value *v = value_of(set, i);
		size_t cap = string_capacity(p);

		b->value[i] = *v;
		if (p->type == TEDGE_PARAM_TYPE_STRING ||
		    p->type == TEDGE_PARAM_TYPE_ENUM) {
			b->value[i].v.s = pool + offset;
			snprintf(pool + offset, cap, "%s",
				 v->v.s != NULL ? v->v.s : "");
			offset += cap;
		}
	}
	return 0;
}

static void backup_restore(const struct param_set *set, const struct backup *b)
{
	for (size_t i = 0; i < set->count && i < b->count; i++) {
		const struct tedge_parameter *p = &set->params[i];
		struct param_value *v = value_of(set, i);

		/* One member at a time: the value is a union, so assigning
		 * the whole struct back would put the backup's string
		 * pointer on top of a scalar. */
		switch (p->type) {
		case TEDGE_PARAM_TYPE_BOOL:
			v->v.b = b->value[i].v.b;
			break;
		case TEDGE_PARAM_TYPE_INT:
			v->v.i = b->value[i].v.i;
			break;
		case TEDGE_PARAM_TYPE_STRING:
		case TEDGE_PARAM_TYPE_ENUM:
			if (v->v.s != NULL) {
				snprintf(v->v.s, string_capacity(p), "%s",
					 b->value[i].v.s);
			}
			break;
		}
	}
}

static bool differs(const struct tedge_parameter *p,
		    const struct param_value *a, const struct param_value *b)
{
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		return a->v.b != b->v.b;
	case TEDGE_PARAM_TYPE_INT:
		return a->v.i != b->v.i;
	case TEDGE_PARAM_TYPE_STRING:
	case TEDGE_PARAM_TYPE_ENUM:
		return strcmp(a->v.s != NULL ? a->v.s : "",
			      b->v.s != NULL ? b->v.s : "") != 0;
	}
	return false;
}

static void assign(const struct tedge_parameter *p, struct param_value *v,
		   const char *raw)
{
	switch (p->type) {
	case TEDGE_PARAM_TYPE_BOOL:
		v->v.b = (strcmp(raw, "true") == 0);
		break;
	case TEDGE_PARAM_TYPE_INT:
		v->v.i = (int32_t)strtol(raw, NULL, 10);
		break;
	case TEDGE_PARAM_TYPE_STRING:
	case TEDGE_PARAM_TYPE_ENUM:
		if (v->v.s != NULL) {
			snprintf(v->v.s, string_capacity(p), "%s", raw);
		}
		break;
	}
}

/* The longest raw value this set can legitimately receive, so a scratch
 * buffer can be sized from the declaration rather than guessed. */
static size_t widest_value(const struct param_set *set)
{
	size_t widest = sizeof("-2147483648");

	for (size_t i = 0; i < set->count; i++) {
		size_t cap = string_capacity(&set->params[i]);

		if (cap > widest) {
			widest = cap;
		}
	}
	return widest;
}

int tedge_params_apply(const char *set_name, const char *json, char *reason,
		       size_t reason_len)
{
	struct param_set *set = find_set(set_name);
	struct backup saved = { 0 };
	char *raw;
	char key[KEY_MAX];
	size_t pos = 0;
	size_t applied = 0;
	int rc;

	if (reason == NULL || reason_len == 0) {
		return -EINVAL;
	}
	reason[0] = '\0';
	if (set == NULL) {
		snprintf(reason, reason_len,
			 "this device declares no parameter set '%s'",
			 set_name != NULL ? set_name : "");
		return -ENOENT;
	}
	if (json == NULL) {
		snprintf(reason, reason_len, "%s: the change carried nothing",
			 set_name);
		return -EINVAL;
	}
	raw = tedge_alloc(widest_value(set));
	if (raw == NULL) {
		snprintf(reason, reason_len, "%s: out of memory", set_name);
		return -ENOMEM;
	}

	/* Pass one: nothing is touched until every value has passed. */
	while (tedge_json_next_member(json, &pos, key, sizeof(key), raw,
				      widest_value(set)) == 1) {
		const struct tedge_parameter *p;
		size_t index;

		p = find_param(set, key, &index);
		if (p == NULL) {
			snprintf(reason, reason_len,
				 "%s: this device does not declare '%s'",
				 set_name, key);
			tedge_free(raw);
			return -ENOENT;
		}
		rc = check_value(p, raw, reason, reason_len);
		if (rc != 0) {
			tedge_free(raw);
			return rc;
		}
		applied++;
	}
	if (applied == 0) {
		snprintf(reason, reason_len, "%s: no values to change",
			 set_name);
		tedge_free(raw);
		return -EINVAL;
	}

	rc = backup_take(set, &saved);
	if (rc != 0) {
		snprintf(reason, reason_len, "%s: out of memory", set_name);
		tedge_free(raw);
		return rc;
	}

	/* Pass two: apply. Every name and value here already passed. */
	pos = 0;
	while (tedge_json_next_member(json, &pos, key, sizeof(key), raw,
				      widest_value(set)) == 1) {
		const struct tedge_parameter *p;
		size_t index;

		p = find_param(set, key, &index);
		if (p != NULL) {
			assign(p, value_of(set, index), raw);
		}
	}
	tedge_free(raw);

	/* The application may still refuse a combination only it understands. */
	if (set->on_change != NULL) {
		rc = set->on_change(set->name, reason, reason_len,
				    set->user_data);
		if (rc != 0) {
			backup_restore(set, &saved);
			backup_free(&saved);
			if (reason[0] == '\0') {
				snprintf(reason, reason_len,
					 "%s: the application refused the change",
					 set_name);
			}
			LOG_WRN("params: %s: rolled back (%s)", set_name,
				reason);
			return rc;
		}
	}

	/* Accepted: write only what actually moved, and report the set. */
	for (size_t i = 0; i < set->count; i++) {
		const struct tedge_parameter *p = &set->params[i];
		const struct param_value *now = value_of(set, i);

		if (!differs(p, now, &saved.value[i])) {
			continue;
		}
		if (store(set, p, now) != 0) {
			LOG_ERR("params: %s/%s: could not be stored", set_name,
				p->name);
		}
	}
	backup_free(&saved);
	publish(set);
	LOG_INF("params: %s: %zu value(s) changed", set_name, applied);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Forgetting                                                                */
/*                                                                           */
/* A firmware update may drop a parameter, or a whole set. What is stored    */
/* for it would otherwise sit in flash for ever, and the next image to       */
/* declare that name again would silently inherit a stranger's value. The    */
/* sweep runs once, from tedge_start(), because that is the first moment     */
/* every declaration is in.                                                  */
/* ------------------------------------------------------------------------ */

/* Deleting while walking the subtree is not safe, so orphans are collected
 * first. A handful is all a firmware update ever leaves behind; anything
 * past that is swept on the next boot. */
#define MAX_ORPHANS 8

struct sweep_ctx {
	char orphan[MAX_ORPHANS][KEY_MAX];
	size_t count;
	bool full;
};

static bool declared(const char *relative)
{
	const char *slash = strchr(relative, '/');
	char set_name[KEY_MAX];
	const struct param_set *set;
	size_t index;

	if (slash == NULL || (size_t)(slash - relative) >= sizeof(set_name)) {
		return false;
	}
	snprintf(set_name, sizeof(set_name), "%.*s", (int)(slash - relative),
		 relative);
	set = find_set(set_name);
	if (set == NULL) {
		return false;
	}
	return find_param(set, slash + 1, &index) != NULL;
}

static int sweep_cb(const char *key, size_t len, settings_read_cb read_cb,
		    void *cb_arg, void *param)
{
	struct sweep_ctx *ctx = param;

	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);

	/* @p key is relative to the subtree: "<set>/<name>". */
	if (declared(key)) {
		return 0;
	}
	if (ctx->count == MAX_ORPHANS) {
		ctx->full = true;
		return 0;
	}
	snprintf(ctx->orphan[ctx->count], KEY_MAX, TEDGE_KEY_PARAM "/%s", key);
	ctx->count++;
	return 0;
}

void tedge_params_on_start(void)
{
	static struct sweep_ctx ctx; /* too big for the caller's stack */

	ctx.count = 0;
	ctx.full = false;
	(void)settings_load_subtree_direct(TEDGE_KEY_PARAM, sweep_cb, &ctx);

	for (size_t i = 0; i < ctx.count; i++) {
		LOG_INF("params: forgetting '%s', which nothing declares now",
			ctx.orphan[i]);
		(void)settings_delete(ctx.orphan[i]);
	}
	if (ctx.full) {
		LOG_WRN("params: more than %d stored values to forget; the "
			"rest go on the next boot", MAX_ORPHANS);
	}
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */

static int lookup(const char *set_name, const char *name,
		  enum tedge_param_type want, const struct param_set **out_set,
		  const struct param_value **out_value)
{
	const struct param_set *set = find_set(set_name);
	const struct tedge_parameter *p;
	size_t index;

	if (set == NULL) {
		return -ENOENT;
	}
	p = find_param(set, name, &index);
	if (p == NULL) {
		return -ENOENT;
	}
	if (p->type != want &&
	    !(want == TEDGE_PARAM_TYPE_STRING &&
	      p->type == TEDGE_PARAM_TYPE_ENUM)) {
		return -EINVAL;
	}
	*out_set = set;
	*out_value = value_of(set, index);
	return 0;
}

int tedge_parameter_get_bool(const char *set, const char *name, bool *out)
{
	const struct param_set *s;
	const struct param_value *v;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}
	rc = lookup(set, name, TEDGE_PARAM_TYPE_BOOL, &s, &v);
	if (rc == 0) {
		*out = v->v.b;
	}
	return rc;
}

int tedge_parameter_get_int(const char *set, const char *name, int32_t *out)
{
	const struct param_set *s;
	const struct param_value *v;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}
	rc = lookup(set, name, TEDGE_PARAM_TYPE_INT, &s, &v);
	if (rc == 0) {
		*out = v->v.i;
	}
	return rc;
}

int tedge_parameter_get_string(const char *set, const char *name, char *out,
			       size_t len)
{
	const struct param_set *s;
	const struct param_value *v;
	int rc;

	if (out == NULL || len == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	rc = lookup(set, name, TEDGE_PARAM_TYPE_STRING, &s, &v);
	if (rc != 0) {
		return rc;
	}
	if (v->v.s == NULL) {
		return -ENOENT;
	}
	if (strlen(v->v.s) >= len) {
		return -ENOMEM;
	}
	strcpy(out, v->v.s);
	return 0;
}
