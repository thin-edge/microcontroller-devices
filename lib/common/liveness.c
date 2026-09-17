/* SPDX-License-Identifier: Apache-2.0
 *
 * Liveness watchdog: one task watchdog channel per watched context, with the
 * SoC watchdog as a hardware fallback, plus a reset record that survives the
 * reset so the next boot can say what stalled. See liveness.h.
 */

#include "liveness.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_system.h>
#endif

LOG_MODULE_REGISTER(app_liveness, CONFIG_LOG_DEFAULT_LEVEL);

#define TIMEOUT_MS   (CONFIG_APP_LIVENESS_TIMEOUT_S * 1000U)
/* The connectivity queue gets its own, longer timeout: Espressif Wi-Fi
 * management and status calls block it for well over 30 s while an access
 * point disappears or returns (measured in the 2026-09-17 AP-restart soak),
 * and resetting for that is a false positive. */
#define NETWQ_TIMEOUT_MS (CONFIG_APP_LIVENESS_NETWQ_TIMEOUT_S * 1000U)
#define FEED_MIN_MS  1000
#define REC_MAGIC    0x4C495645U /* "LIVE" */
#define REC_NONE     0xFFU

/* Kept across a reset (not across power loss). On Espressif SoCs, RTC slow
 * memory is the region that survives software and watchdog resets; elsewhere
 * the generic no-init RAM section. Validated by magic + check word, so the
 * garbage found after power-on is ignored. */
struct reset_record {
	uint32_t magic;
	uint32_t boots;     /* boots since the record was last (re)initialised */
	uint32_t ctx;       /* stalled context, or REC_NONE */
	uint32_t uptime_s;  /* uptime at the stall */
	char step[16];      /* what the context was doing, copied (not a pointer:
			     * a reflash would leave a pointer dangling) */
	uint32_t check;
};

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
static struct reset_record rec __attribute__((section(".rtc_noinit.app_liveness")));
#else
static struct reset_record rec __noinit;
#endif

static uint32_t rec_check(const struct reset_record *r)
{
	uint32_t h = r->magic ^ r->boots ^ (r->ctx << 8) ^ r->uptime_s;

	for (size_t i = 0; i < sizeof(r->step); i++) {
		h = (h << 1) ^ (uint8_t)r->step[i];
	}
	return ~h;
}

static void rec_seal(void)
{
	rec.check = rec_check(&rec);
}

#define BOOT_LINES 3
static char boot_line[BOOT_LINES][17];
static size_t boot_line_count;

static int chan[APP_CTX_COUNT] = { [0 ... APP_CTX_COUNT - 1] = -1 };
static int64_t last_feed[APP_CTX_COUNT];
static bool ready;

#if defined(CONFIG_APP_LIVENESS_SELFTEST_STOP_PROTO)
static atomic_t selftest_stop_proto;
#endif
#if defined(CONFIG_APP_LIVENESS_SELFTEST)
/* Never given: whatever takes it blocks for good. A named object, so the diag
 * STALE report's pended_on address can be checked against `nm`. */
static K_SEM_DEFINE(liveness_selftest_block_sem, 0, 1);
#endif

static uint32_t ctx_timeout_ms(enum app_ctx ctx)
{
	return (ctx == APP_CTX_NETWQ) ? NETWQ_TIMEOUT_MS : TIMEOUT_MS;
}

/* The expiry callback runs in the task watchdog's timer ISR, where logging a
 * thread dump is unreliable (with deferred logging only the first line came
 * out). It hands over to this cooperative thread, which logs and resets. If
 * the system is too wedged to schedule it, the hardware watchdog resets the
 * device a few seconds later and the boot log says so. */
static K_SEM_DEFINE(reap_sem, 0, 1);
static atomic_t reap_ctx = ATOMIC_INIT(APP_CTX_COUNT);

static void reaper_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&reap_sem, K_FOREVER);

		enum app_ctx ctx = (enum app_ctx)atomic_get(&reap_ctx);

		LOG_PANIC(); /* synchronous output from here on */
		LOG_ERR("liveness: %s made no progress for %u s in step '%s' — resetting",
			app_ctx_name(ctx), ctx_timeout_ms(ctx) / 1000U,
			app_step_get(ctx));
		app_diag_dump_threads();
		sys_reboot(SYS_REBOOT_COLD);
	}
}

K_THREAD_DEFINE(app_liveness_reaper, 2048, reaper_main, NULL, NULL, NULL,
		K_PRIO_COOP(1), 0, 0);

/* Runs in the task watchdog's timer ISR. */
static void channel_expired(int channel_id, void *user_data)
{
	uintptr_t ctx = (uintptr_t)user_data;

	ARG_UNUSED(channel_id);

	rec.ctx = (uint32_t)ctx;
	rec.uptime_s = k_uptime_seconds();
	strncpy(rec.step, app_step_get((enum app_ctx)ctx), sizeof(rec.step) - 1);
	rec.step[sizeof(rec.step) - 1] = '\0';
	rec_seal();

	atomic_set(&reap_ctx, (atomic_val_t)ctx);
	k_sem_give(&reap_sem);
}

void app_liveness_feed(enum app_ctx ctx)
{
	if (!ready || ctx >= APP_CTX_COUNT) {
		return;
	}

	int64_t now = k_uptime_get();

	if (chan[ctx] < 0) {
		/* First progress from this context: start watching it. */
		chan[ctx] = task_wdt_add(ctx_timeout_ms(ctx), channel_expired,
					 (void *)(uintptr_t)ctx);
		if (chan[ctx] < 0) {
			LOG_ERR("task_wdt_add(%s) failed (%d)", app_ctx_name(ctx),
				chan[ctx]);
			ready = false;
			return;
		}
		last_feed[ctx] = now;
		LOG_INF("watching %s (timeout %u s)", app_ctx_name(ctx),
			ctx_timeout_ms(ctx) / 1000U);
		return;
	}

#if defined(CONFIG_APP_LIVENESS_SELFTEST_STOP_PROTO)
	if (ctx == APP_CTX_PROTO && atomic_get(&selftest_stop_proto)) {
		LOG_WRN("SELFTEST: protocol loop stops here");
		(void)k_sem_take(&liveness_selftest_block_sem, K_FOREVER);
	}
#endif

	if (now - last_feed[ctx] < FEED_MIN_MS) {
		return;
	}
	last_feed[ctx] = now;
	(void)task_wdt_feed(chan[ctx]);
}

size_t app_liveness_boot_lines(const char **lines, size_t max)
{
	size_t n = MIN(boot_line_count, max);

	for (size_t i = 0; i < n; i++) {
		lines[i] = boot_line[i];
	}
	return n;
}

static const char *cause_str(uint32_t cause)
{
	if (cause & RESET_POR) {
		return "power-on";
	}
	if (cause & RESET_BROWNOUT) {
		return "brownout";
	}
	if (cause & RESET_WATCHDOG) {
		return "watchdog";
	}
	if (cause & RESET_CPU_LOCKUP) {
		return "panic";
	}
	if (cause & RESET_SOFTWARE) {
		return "software";
	}
	if (cause & RESET_PIN) {
		return "pin";
	}
	return "other";
}

static int liveness_init(void)
{
	uint32_t cause = 0;
	int esp_reason = -1;

	(void)hwinfo_get_reset_cause(&cause);
	(void)hwinfo_clear_reset_cause();
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
	esp_reason = (int)esp_reset_reason();
#endif

	bool valid = rec.magic == REC_MAGIC && rec.check == rec_check(&rec) &&
		     !(cause & RESET_POR);

	if (!valid) {
		rec.magic = REC_MAGIC;
		rec.boots = 0;
		rec.ctx = REC_NONE;
		rec.uptime_s = 0;
		rec.step[0] = '\0';
	}
	rec.boots++;

	if (rec.ctx != REC_NONE) {
		LOG_ERR("LIVENESS RESET: context %s stalled in step '%s' at uptime %u s (boot %u)",
			app_ctx_name(rec.ctx), rec.step, rec.uptime_s, rec.boots);
	} else if (cause & RESET_WATCHDOG) {
		LOG_ERR("hardware watchdog reset: no liveness record, the task "
			"watchdog never ran (boot %u)", rec.boots);
	}
	LOG_INF("reset cause 0x%x (%s) esp_reason %d boot %u", cause,
		cause_str(cause), esp_reason, rec.boots);

	/* r<n>: the SoC's own reset reason (esp_reset_reason() on Espressif,
	 * -1 elsewhere), finer than the hwinfo cause. */
	snprintf(boot_line[0], sizeof(boot_line[0]), "BOOT b%u r%d", rec.boots,
		 esp_reason);
	snprintf(boot_line[1], sizeof(boot_line[1]), "CAUSE %s",
		 (cause & RESET_WATCHDOG) ? "hw-wdt" : cause_str(cause));
	boot_line_count = 2;
	if (rec.ctx != REC_NONE) {
		snprintf(boot_line[2], sizeof(boot_line[2]), "STALL %s %.6s",
			 app_ctx_name(rec.ctx), rec.step);
		boot_line_count = 3;
	}

	rec.ctx = REC_NONE;
	rec.uptime_s = 0;
	memset(rec.step, 0, sizeof(rec.step));
	rec_seal();

	const struct device *hw_wdt = NULL;

#if defined(CONFIG_TASK_WDT_HW_FALLBACK) && DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(watchdog0))
	hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	if (!device_is_ready(hw_wdt)) {
		LOG_WRN("watchdog0 not ready — no hardware fallback");
		hw_wdt = NULL;
	}
#endif

	int rc = task_wdt_init(hw_wdt);

	if (rc != 0) {
		LOG_ERR("task_wdt_init failed (%d) — liveness watchdog off", rc);
		return 0;
	}
	ready = true;
	LOG_INF("liveness watchdog armed (timeout %u s, netwq %u s, hw fallback %s)",
		CONFIG_APP_LIVENESS_TIMEOUT_S, CONFIG_APP_LIVENESS_NETWQ_TIMEOUT_S,
		hw_wdt ? "on" : "off");
	return 0;
}

SYS_INIT(liveness_init, APPLICATION, 50);

#if defined(CONFIG_APP_LIVENESS_SELFTEST)

#if defined(CONFIG_APP_LIVENESS_SELFTEST_BLOCK_WQ)
static void block_wq_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("SELFTEST: system workqueue blocks here");
	(void)k_sem_take(&liveness_selftest_block_sem, K_FOREVER);
}

static K_WORK_DEFINE(block_wq_work, block_wq_handler);
#endif

static void selftest_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#if defined(CONFIG_APP_LIVENESS_SELFTEST_BLOCK_WQ)
	LOG_WRN("SELFTEST: injecting a blocked system workqueue");
	k_work_submit(&block_wq_work);
#elif defined(CONFIG_APP_LIVENESS_SELFTEST_STOP_PROTO)
	LOG_WRN("SELFTEST: injecting a stopped protocol loop");
	atomic_set(&selftest_stop_proto, 1);
#elif defined(CONFIG_APP_LIVENESS_SELFTEST_IRQ_LOCK)
	LOG_WRN("SELFTEST: injecting a busy loop with interrupts masked");
	LOG_PANIC();
	(void)irq_lock();
	for (;;) {
		/* only the hardware watchdog gets us out of here */
	}
#endif
}

/* Cooperative top priority, so the IRQ-lock case really owns the CPU. */
K_THREAD_DEFINE(liveness_selftest, 1536, selftest_main, NULL, NULL, NULL,
		K_PRIO_COOP(0), 0, CONFIG_APP_LIVENESS_SELFTEST_DELAY_S * 1000);

#endif /* CONFIG_APP_LIVENESS_SELFTEST */
