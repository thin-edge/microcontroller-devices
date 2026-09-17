/* SPDX-License-Identifier: Apache-2.0
 *
 * Liveness watchdog (CONFIG_APP_LIVENESS) and the shared progress hook.
 *
 * Every watched context calls app_alive() when it has made genuine progress:
 * the system workqueue from net.c's status tick, each protocol server from its
 * loop. With CONFIG_APP_LIVENESS each context gets its own task watchdog
 * channel (registered on its first call), backed by the SoC hardware watchdog
 * (the `watchdog0` alias); a context that stops calling app_alive() for
 * CONFIG_APP_LIVENESS_TIMEOUT_S resets the device, and the next boot logs which
 * context it was. With diagnostics on, the same call feeds the health line.
 * With both options off, app_alive() compiles to nothing.
 */
#ifndef APP_LIVENESS_H_
#define APP_LIVENESS_H_

#include "diag.h"

#if defined(CONFIG_APP_LIVENESS)

/** Feed @p ctx's watchdog channel (rate-limited; cheap to call often). */
void app_liveness_feed(enum app_ctx ctx);

/**
 * Why this boot happened, as short lines (at most 16 characters, the width of
 * the Feather TFT) for the status display, since a board without a serial
 * console cannot show the boot log: "BOOT b<boots> r<SoC reason>", "CAUSE
 * <cause>" and, after a liveness reset, "STALL <ctx> u<uptime>".
 *
 * @return the number of lines (0 before the watchdog has initialised).
 */
size_t app_liveness_boot_lines(const char **lines, size_t max);

#else

static inline void app_liveness_feed(enum app_ctx ctx)
{
	ARG_UNUSED(ctx);
}

static inline size_t app_liveness_boot_lines(const char **lines, size_t max)
{
	ARG_UNUSED(lines);
	ARG_UNUSED(max);
	return 0;
}

#endif /* CONFIG_APP_LIVENESS */

#if defined(CONFIG_APP_DIAG) || defined(CONFIG_APP_LIVENESS)

/**
 * Record what @p ctx is doing now, as a string literal with a static lifetime
 * ("ping", "wifi-connect", ...). Stall reports and the liveness reset message
 * name it, which is how a blocking driver call gets identified.
 */
void app_step(enum app_ctx ctx, const char *what);

/** The last step recorded for @p ctx, or "?". */
const char *app_step_get(enum app_ctx ctx);

#else

static inline void app_step(enum app_ctx ctx, const char *what)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(what);
}

static inline const char *app_step_get(enum app_ctx ctx)
{
	ARG_UNUSED(ctx);
	return "?";
}

#endif

/**
 * Progress hook for a watched context. Call it from the context's own thread
 * each time round its loop, and only when the loop really ran (a receive that
 * returned or timed out, a completed tick), never from a timer.
 */
static inline void app_alive(enum app_ctx ctx)
{
	app_diag_beat(ctx);
	app_liveness_feed(ctx);
}

#endif /* APP_LIVENESS_H_ */
