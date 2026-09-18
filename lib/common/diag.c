/* SPDX-License-Identifier: Apache-2.0
 *
 * Liveness and resource-health diagnostics. See diag.h and the "Reading
 * diagnostic output" section of the esp32-network-freeze-investigation
 * evidence for the line format.
 */

#include "liveness.h"
#include "net.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>
#include <stdio.h>

#if defined(CONFIG_NET_NATIVE)
#include <zephyr/net/net_pkt.h>
#include <zephyr/net_buf.h>
#endif

#if defined(CONFIG_WIFI)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

LOG_MODULE_REGISTER(app_diag, CONFIG_LOG_DEFAULT_LEVEL);

#define PERIOD_MS   (CONFIG_APP_DIAG_PERIOD_S * 1000U)
/* Two periods (20 s by default): early enough that the report is printed
 * before the liveness watchdog (30 s by default) resets the device. */
#define STALE_MS    (2U * PERIOD_MS)
#define MAX_WORKS   6
#define STACK_REPORT_EVERY MAX(1U, 60U / CONFIG_APP_DIAG_PERIOD_S)

/* Uptime (ms) of each context's last beat; 0 = never beat. */
static atomic_t beat_ms[APP_CTX_COUNT];
static struct k_thread *beat_thread[APP_CTX_COUNT];

static struct {
	const char *name;
	struct k_work_delayable *work;
} works[MAX_WORKS];
static size_t n_works;
static struct k_spinlock works_lock;

void app_diag_beat(enum app_ctx ctx)
{
	if (ctx >= APP_CTX_COUNT) {
		return;
	}
	/* | 1 keeps a beat at uptime 0 distinguishable from "never". */
	atomic_set(&beat_ms[ctx], (atomic_val_t)(k_uptime_get_32() | 1U));
	beat_thread[ctx] = k_current_get();
}

void app_diag_watch_work(const char *name, struct k_work_delayable *work)
{
	k_spinlock_key_t key = k_spin_lock(&works_lock);
	bool full = n_works >= MAX_WORKS;

	if (!full) {
		works[n_works].work = work;
		works[n_works].name = name;
		n_works++;
	}
	k_spin_unlock(&works_lock, key);

	if (full) {
		LOG_WRN("too many watched work items; '%s' not reported", name);
	}
}

static const char *thread_label(const struct k_thread *t, char *buf, size_t n)
{
	const char *name = k_thread_name_get((k_tid_t)t);

	if (name != NULL && name[0] != '\0') {
		return name;
	}
	snprintf(buf, n, "%p", (void *)t);
	return buf;
}

static void dump_one(const struct k_thread *t, void *user_data)
{
	bool routine = user_data != NULL;
	char label[16];
	char state[32];
	size_t unused = 0;
	int stack_rc = -ENOTSUP;

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	stack_rc = k_thread_stack_space_get(t, &unused);
#endif
#if defined(CONFIG_THREAD_STACK_INFO)
	if (routine) {
		LOG_INF("  thread %s stack_unused %d of %zu",
			thread_label(t, label, sizeof(label)),
			stack_rc == 0 ? (int)unused : -1, t->stack_info.size);
		return;
	}
#else
	ARG_UNUSED(routine);
#endif
	LOG_WRN("  thread %s prio %d state %s pended_on %p stack_unused %d",
		thread_label(t, label, sizeof(label)), t->base.prio,
		k_thread_state_str((k_tid_t)t, state, sizeof(state)),
		(void *)t->base.pended_on,
		stack_rc == 0 ? (int)unused : -1);
}

void app_diag_dump_threads(void)
{
	LOG_WRN("thread dump (current %p):", (void *)k_current_get());
	k_thread_foreach_unlocked(dump_one, NULL);
}

static char work_flag(struct k_work_delayable *w)
{
	int busy = k_work_delayable_busy_get(w);

	if (busy & K_WORK_RUNNING) {
		return 'R';
	}
	if (busy & K_WORK_CANCELING) {
		return 'C';
	}
	if (busy & (K_WORK_QUEUED)) {
		return 'Q';
	}
	if (busy & K_WORK_DELAYED) {
		return 'D';
	}
	return '-';
}

/* Append to buf at *pos, never overrunning. */
#define APPEND(buf, pos, ...)                                                  \
	do {                                                                   \
		int _w = snprintf((buf) + *(pos), sizeof(buf) - *(pos),        \
				  __VA_ARGS__);                                \
		if (_w > 0) {                                                  \
			*(pos) = MIN(sizeof(buf) - 1, *(pos) + (size_t)_w);    \
		}                                                              \
	} while (0)

#if (K_HEAP_MEM_POOL_SIZE > 0) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct k_heap _system_heap;
#endif

#if defined(CONFIG_COMMON_LIBC_MALLOC) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && \
	(CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE != 0)
#define HAVE_MALLOC_STATS 1
int malloc_runtime_stats_get(struct sys_memory_stats *stats);
#endif

/* Age of a beat taken at uptime b. Another thread may beat between our reading
 * of `now` and of `b`, so b can be slightly newer than now: that is age 0, not a
 * wrapped 49-day age. */
static uint32_t beat_age_ms(uint32_t now, uint32_t b)
{
	return (b > now) ? 0U : now - b;
}

static void log_health(uint32_t now, uint32_t n)
{
	char line[256];
	size_t pos = 0;

	APPEND(line, &pos, "HEALTH up=%u n=%u beat[", now / 1000U, n);
	for (int c = 0; c < APP_CTX_COUNT; c++) {
		uint32_t b = (uint32_t)atomic_get(&beat_ms[c]);

		if (b == 0) {
			APPEND(line, &pos, "%s%s=-", c ? " " : "", app_ctx_name(c));
		} else {
			APPEND(line, &pos, "%s%s=%u", c ? " " : "", app_ctx_name(c),
			       beat_age_ms(now, b) / 1000U);
		}
	}
	APPEND(line, &pos, "] work[");
	/* Entries are only ever appended, and n_works is bumped after the entry
	 * is filled in, so reading without the lock is safe. */
	for (size_t i = 0; i < n_works; i++) {
		APPEND(line, &pos, "%s%s=%c", i ? " " : "", works[i].name,
		       work_flag(works[i].work));
	}
	APPEND(line, &pos, "]");

#if (K_HEAP_MEM_POOL_SIZE > 0) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	{
		struct sys_memory_stats st;

		if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) == 0) {
			APPEND(line, &pos, " heap=%zu/%zu", st.free_bytes,
			       st.free_bytes + st.allocated_bytes);
		}
	}
#endif
#if defined(HAVE_MALLOC_STATS)
	{
		struct sys_memory_stats st;

		if (malloc_runtime_stats_get(&st) == 0) {
			APPEND(line, &pos, " malloc=%zu/%zu", st.free_bytes,
			       st.free_bytes + st.allocated_bytes);
		}
	}
#endif

#if defined(CONFIG_NET_NATIVE)
	{
		struct k_mem_slab *rx, *tx;
		struct net_buf_pool *rxd, *txd;

		net_pkt_get_info(&rx, &tx, &rxd, &txd);
		APPEND(line, &pos, " pkt[rx=%u/%u tx=%u/%u]",
		       k_mem_slab_num_free_get(rx), rx->info.num_blocks,
		       k_mem_slab_num_free_get(tx), tx->info.num_blocks);
#if defined(CONFIG_NET_BUF_POOL_USAGE)
		APPEND(line, &pos, " buf[rx=%zu/%u tx=%zu/%u] min[rx=%zu tx=%zu]",
		       net_buf_get_available(rxd), rxd->buf_count,
		       net_buf_get_available(txd), txd->buf_count,
		       rxd->buf_count - net_buf_get_max_used(rxd),
		       txd->buf_count - net_buf_get_max_used(txd));
#endif
	}
#endif
	APPEND(line, &pos, " net=%s/%s", app_net_is_connected() ? "up" : "down",
	       app_step_get(APP_CTX_NETWQ));

	LOG_INF("%s", line);

#if defined(CONFIG_WIFI)
	/* Separate line, logged after the core one: if the Wi-Fi driver is wedged
	 * this query can block, and a HEALTH line with no WIFI line after it is
	 * then the evidence. */
	struct net_if *wifi = net_if_get_first_wifi();
	struct wifi_iface_status st = { 0 };

	if (wifi != NULL &&
	    net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi, &st, sizeof(st)) == 0) {
		LOG_INF("WIFI st=%d rssi=%d ch=%u", st.state, st.rssi, st.channel);
	} else {
		LOG_INF("WIFI status unavailable");
	}
#endif
}

static void check_stale(uint32_t now)
{
	static bool was_stale[APP_CTX_COUNT];

	for (int c = 0; c < APP_CTX_COUNT; c++) {
		uint32_t b = (uint32_t)atomic_get(&beat_ms[c]);
		bool stale = b != 0 && beat_age_ms(now, b) > STALE_MS;

		if (stale) {
			struct k_thread *t = beat_thread[c];
			char label[16];
			char state[32];

			LOG_WRN("STALE %s age=%us step='%s' thread=%s state=%s pended_on=%p",
				app_ctx_name(c), beat_age_ms(now, b) / 1000U,
				app_step_get((enum app_ctx)c),
				thread_label(t, label, sizeof(label)),
				k_thread_state_str(t, state, sizeof(state)),
				(void *)t->base.pended_on);
			if (!was_stale[c]) {
				app_diag_dump_threads();
			}
		} else if (was_stale[c]) {
			LOG_WRN("RECOVERED %s", app_ctx_name(c));
		}
		was_stale[c] = stale;
	}
}

static void diag_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (uint32_t n = 1;; n++) {
		k_sleep(K_MSEC(PERIOD_MS));

		uint32_t now = k_uptime_get_32();

		check_stale(now);
		log_health(now, n);
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
		/* Stack headroom once a minute (at the default period): an overflow
		 * corrupts memory silently, so watch the margin before it runs out. */
		if (n % STACK_REPORT_EVERY == 1) {
			LOG_INF("STACKS (unused bytes, lowest seen):");
			k_thread_foreach_unlocked(dump_one, (void *)1);
		}
#endif
	}
}

/* Preemptible but above every app thread (their priorities are 5..7), so a
 * busy protocol thread cannot silence it. A spinning cooperative thread (the
 * system workqueue) still can; the liveness watchdog covers that case. */
K_THREAD_DEFINE(app_diag_thread, CONFIG_APP_DIAG_STACK_SIZE, diag_main,
		NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, 0);
