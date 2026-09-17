/* SPDX-License-Identifier: Apache-2.0
 *
 * Progress tracking shared by the diagnostics and the liveness watchdog.
 *
 * - A system workqueue probe. None of the firmware's own periodic work runs on
 *   the system workqueue any more (connectivity has its own queue), but the
 *   Zephyr network stack and the simulations still use it. This work item
 *   reschedules itself every few seconds; each time the queue gets round to it,
 *   that is genuine progress of the system workqueue, reported via app_alive().
 * - The current step of each watched context (app_step()), so a stall report
 *   names what the context was doing, not just that it stopped.
 *
 * Built only with diagnostics or the liveness watchdog.
 */

#include "liveness.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#define PROBE_PERIOD K_SECONDS(3)

/* Written by the context itself, read by the diagnostics thread and by the
 * liveness reaper. A plain pointer to a string literal: the write is atomic on
 * every supported target, and a torn read is impossible. */
static const char *step[APP_CTX_COUNT];

void app_step(enum app_ctx ctx, const char *what)
{
	if (ctx < APP_CTX_COUNT) {
		step[ctx] = what;
	}
}

const char *app_step_get(enum app_ctx ctx)
{
	const char *s = (ctx < APP_CTX_COUNT) ? step[ctx] : NULL;

	return (s != NULL) ? s : "?";
}

static struct k_work_delayable probe_work;

static void probe_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	app_step(APP_CTX_SYSWQ, "probe");
	app_alive(APP_CTX_SYSWQ);
	(void)k_work_reschedule(&probe_work, PROBE_PERIOD);
}

static int probe_init(void)
{
	k_work_init_delayable(&probe_work, probe_handler);
	(void)k_work_reschedule(&probe_work, PROBE_PERIOD);
	app_diag_watch_work("probe", &probe_work);
	return 0;
}

/* After liveness_init (APPLICATION 50), so the first beat can register. */
SYS_INIT(probe_init, APPLICATION, 60);
