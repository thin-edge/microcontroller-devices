/* SPDX-License-Identifier: Apache-2.0
 *
 * Liveness and resource-health diagnostics (CONFIG_APP_DIAG).
 *
 * Watched contexts call app_alive() (liveness.h) each time they make progress.
 * With diagnostics enabled, a dedicated thread (not on the system workqueue)
 * periodically logs how long ago each context last made progress, which of
 * the firmware's own work items are running, and heap / net buffer / Wi-Fi
 * health. When a context has made no progress for two periods it also dumps
 * every thread's state and the wait queue it is blocked on. With
 * CONFIG_APP_DIAG=n every function here is an empty inline and nothing is
 * linked in.
 */
#ifndef APP_DIAG_H_
#define APP_DIAG_H_

#include <zephyr/kernel.h>

/** Contexts that must keep making progress. */
enum app_ctx {
	APP_CTX_SYSWQ, /**< System workqueue (periodic probe work item). */
	APP_CTX_NETWQ, /**< Connectivity work queue (net.c status tick). */
	APP_CTX_PROTO, /**< Protocol server thread (OPC-UA / Modbus / SNMP agent). */
	APP_CTX_TRAP,  /**< SNMP trap sender thread. */
	APP_CTX_COUNT,
};

/** Short name of a context, as printed in health lines and reset records. */
static inline const char *app_ctx_name(unsigned int ctx)
{
	static const char *const names[APP_CTX_COUNT] = {
		"wq", "netwq", "proto", "trap",
	};

	return ctx < APP_CTX_COUNT ? names[ctx] : "?";
}

#if defined(CONFIG_APP_DIAG)

/** Record that @p ctx made progress (and which thread it runs on). */
void app_diag_beat(enum app_ctx ctx);

/** Report the busy state of a firmware work item in every health line. */
void app_diag_watch_work(const char *name, struct k_work_delayable *work);

/** Log every thread's name, priority, state and wait queue. ISR-safe. */
void app_diag_dump_threads(void);

#else

static inline void app_diag_beat(enum app_ctx ctx)
{
	ARG_UNUSED(ctx);
}

static inline void app_diag_watch_work(const char *name,
				       struct k_work_delayable *work)
{
	ARG_UNUSED(name);
	ARG_UNUSED(work);
}

static inline void app_diag_dump_threads(void) { }

#endif /* CONFIG_APP_DIAG */

#endif /* APP_DIAG_H_ */
