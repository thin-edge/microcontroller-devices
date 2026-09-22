/* SPDX-License-Identifier: Apache-2.0
 *
 * Deciding whether a command from the cloud may run.
 *
 * The rule is deliberately narrow: a request must begin with one of the
 * prefixes the integrator listed, and whatever follows must be arguments.
 * Anything that could turn one command into two is refused before the list
 * is even consulted, because an allowed prefix must not become a doorway.
 *
 * Pure, and unit-tested on native_sim: this is the function that decides
 * what a remote party may do to the device, so it is worth testing without
 * a board.
 */

#include "tedge_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Characters a Zephyr shell line may not carry here. The shell itself
 * supports none of the redirection a POSIX shell does, but the list costs
 * nothing and survives a future shell that does. */
static const char forbidden[] = ";|&`$><\n\r";

static const char *skip_spaces(const char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

bool tedge_shell_command_allowed(const char *list, const char *cmd,
				 const char **why)
{
	const char *entry;

	if (why != NULL) {
		*why = "";
	}
	if (cmd == NULL || *skip_spaces(cmd) == '\0') {
		if (why != NULL) {
			*why = "the command is empty";
		}
		return false;
	}
	if (strpbrk(cmd, forbidden) != NULL) {
		if (why != NULL) {
			*why = "the command contains a character that could "
			       "chain another command";
		}
		return false;
	}
	if (list == NULL || *skip_spaces(list) == '\0') {
		if (why != NULL) {
			*why = "this device runs no commands: its allow-list "
			       "(CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST) is empty";
		}
		return false;
	}

	cmd = skip_spaces(cmd);
	for (entry = skip_spaces(list); *entry != '\0';) {
		const char *end = strchr(entry, ',');
		size_t len = (end != NULL) ? (size_t)(end - entry)
					   : strlen(entry);

		/* Trim trailing spaces of this entry. */
		while (len > 0 && (entry[len - 1] == ' ' ||
				   entry[len - 1] == '\t')) {
			len--;
		}
		if (len > 0 && strncmp(cmd, entry, len) == 0 &&
		    (cmd[len] == '\0' || cmd[len] == ' ')) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		entry = skip_spaces(end + 1);
	}
	if (why != NULL) {
		*why = "this command is not on the device's allow-list";
	}
	return false;
}

bool tedge_shell_is_help(const char *cmd)
{
	size_t len;

	if (cmd == NULL) {
		return false;
	}
	cmd = skip_spaces(cmd);
	len = strlen(cmd);
	while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) {
		len--;
	}
	return (len == 4 && strncmp(cmd, "help", 4) == 0) ||
	       (len == 1 && cmd[0] == '?');
}

int tedge_shell_help_text(const char *list, char *buf, size_t len)
{
	const char *entry;
	size_t n = 0;
	int count = 0;

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}
	buf[0] = '\0';
	for (entry = (list != NULL) ? skip_spaces(list) : "";
	     *entry != '\0';) {
		const char *end = strchr(entry, ',');
		size_t elen = (end != NULL) ? (size_t)(end - entry)
					    : strlen(entry);

		while (elen > 0 && (entry[elen - 1] == ' ' ||
				    entry[elen - 1] == '\t')) {
			elen--;
		}
		if (elen > 0) {
			if (count == 0) {
				n += snprintf(buf + n, len - n, "%s",
					      "Commands this device runs "
					      "(arguments may follow):");
			}
			if (n < len) {
				n += snprintf(buf + n, len - n, "\n  %.*s",
					      (int)elen, entry);
			}
			count++;
		}
		if (end == NULL || n >= len) {
			break;
		}
		entry = skip_spaces(end + 1);
	}
	if (count == 0) {
		snprintf(buf, len, "%s", "This device runs no commands: its "
			 "allow-list (CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST) is "
			 "empty.");
	}
	return (n >= len) ? -ENOSPC : count;
}
