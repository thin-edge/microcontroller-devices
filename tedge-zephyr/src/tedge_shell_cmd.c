/* SPDX-License-Identifier: Apache-2.0
 *
 * Running a command the cloud asked for.
 *
 * Two things this deliberately is not. It is not a shell on the network:
 * Zephyr's telnet shell binds every interface, mirrors the log into the
 * session and cannot turn echo off, so the command runs in-process against
 * the dummy backend Zephyr provides for capturing output. And it is not run
 * on the client's thread: the shell thread prints deferred log messages, so
 * a command that takes a while would hold the connection up. It gets a
 * thread of its own, as the download and the tunnel do.
 *
 * What may run is decided by tedge_shell_allow_list.c, which refuses
 * everything until the integrator says otherwise.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

static K_THREAD_STACK_DEFINE(cmd_stack, CONFIG_TEDGE_SHELL_COMMAND_STACK_SIZE);
static struct k_thread cmd_thread;

static struct {
	char cmd[160];
	int64_t started;
	bool running;
	bool reported;
} job;

K_MSGQ_DEFINE(cmd_events, sizeof(struct tedge_shell_event), 2, 4);

/* ------------------------------------------------------------------------ */
/* Running it                                                                */
/* ------------------------------------------------------------------------ */

static void run_thread(void *a, void *b, void *c)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	struct tedge_shell_event ev = { 0 };
	const char *out;
	size_t len = 0;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	shell_backend_dummy_clear_output(sh);
	ret = shell_execute_cmd(sh, job.cmd);
	out = shell_backend_dummy_get_output(sh, &len);

	ev.rc = ret;
	if (out != NULL && len > 0) {
		size_t keep = MIN(len, sizeof(ev.output) - 1);

		memcpy(ev.output, out, keep);
		ev.output[keep] = '\0';
	}
	if (ret != 0 && ev.output[0] == '\0') {
		snprintf(ev.output, sizeof(ev.output), "the command failed (%d)",
			 ret);
	}
	(void)k_msgq_put(&cmd_events, &ev, K_NO_WAIT);
	job.running = false;
}

/* ------------------------------------------------------------------------ */
/* The request                                                               */
/* ------------------------------------------------------------------------ */

int tedge_shell_request(const char *line, char *reason, size_t rlen)
{
	char cmd[160];
	const char *why = "";

	/* 511,<device>,<command> */
	if (tedge_sr_field(line, 2, cmd, sizeof(cmd)) <= 0) {
		snprintf(reason, rlen, "malformed command");
		return -EINVAL;
	}
	if (job.running) {
		snprintf(reason, rlen, "a command is already running");
		return -EBUSY;
	}
	if (!tedge_shell_command_allowed(CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST,
					 cmd, &why)) {
		/* The command itself is not logged at INF: a refused command
		 * is someone else's text, and it lands in the cloud anyway. */
		LOG_WRN("shell: refused a command: %s", why);
		snprintf(reason, rlen, "%s", why);
		return -EACCES;
	}

	snprintf(job.cmd, sizeof(job.cmd), "%s", cmd);
	job.started = k_uptime_get();
	job.running = true;
	job.reported = false;
	LOG_INF("shell: running \"%s\"", job.cmd);
	k_thread_create(&cmd_thread, cmd_stack, K_THREAD_STACK_SIZEOF(cmd_stack),
			run_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY), 0,
			K_NO_WAIT);
	k_thread_name_set(&cmd_thread, "tedge_cmd");
	return 0;
}

int tedge_shell_poll_event(struct tedge_shell_event *ev)
{
	if (k_msgq_get(&cmd_events, ev, K_NO_WAIT) == 0) {
		return 0;
	}
	/* Zephyr cannot interrupt a running command, so a timeout reports the
	 * operation as failed and leaves the thread to finish. The next
	 * command is refused until it does, which is the honest state. */
	if (job.running && !job.reported &&
	    k_uptime_get() - job.started >
		    CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S * 1000LL) {
		job.reported = true;
		memset(ev, 0, sizeof(*ev));
		ev->rc = -ETIMEDOUT;
		snprintf(ev->output, sizeof(ev->output),
			 "the command did not finish within %d s and is still "
			 "running",
			 CONFIG_TEDGE_SHELL_COMMAND_TIMEOUT_S);
		return 0;
	}
	return -ENOMSG;
}
