/* SPDX-License-Identifier: Apache-2.0
 *
 * Lifecycle and state machine of the client: one thread that waits for the
 * application's network, sets the clock, lets the transport connect, and
 * services the session until it ends, then reconnects with back-off.
 *
 *   STOPPED -> WAITING_NETWORK -> WAITING_TIME -> [AWAITING_REGISTRATION]
 *           -> CONNECTING -> CONNECTED -> (loss) -> back-off -> CONNECTING
 *
 * The module never brings up or configures the network: it only listens for
 * connectivity events (tedge-client-module: "The application owns
 * connectivity").
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define BACKOFF_MIN_S 3
#define STABLE_S      60
/* Twin fragments the application can hold at once, plus the module's own. */
#define TWIN_SLOTS    4

K_HEAP_DEFINE(tedge_heap, CONFIG_TEDGE_HEAP_SIZE);

static struct tedge_id identity;
static const struct tedge_hooks *hooks;
static atomic_t state = ATOMIC_INIT(TEDGE_STATE_STOPPED);
static bool initialised;

static K_EVENT_DEFINE(events);
#define EV_NET_UP   BIT(0)
#define EV_NET_DOWN BIT(1)
#define EV_STOP     BIT(2)
#define EV_WAKE     BIT(3)

static struct net_mgmt_event_callback l4_cb;
static char c8y_host[80];

/* Twin fragments, republished after every reconnect. */
static struct {
	char fragment[32];
	char *json; /* module heap */
} twin[TWIN_SLOTS];
static K_MUTEX_DEFINE(twin_lock);
static bool twin_dirty;

/* ------------------------------------------------------------------------ */
/* Heap                                                                      */
/* ------------------------------------------------------------------------ */

void *tedge_alloc(size_t size)
{
	void *p = k_heap_alloc(&tedge_heap, size, K_NO_WAIT);

	if (p == NULL) {
		LOG_ERR("out of client heap (%zu B wanted of %d B; raise "
			"CONFIG_TEDGE_HEAP_SIZE)", size, CONFIG_TEDGE_HEAP_SIZE);
	}
	return p;
}

void tedge_free(void *p)
{
	if (p != NULL) {
		k_heap_free(&tedge_heap, p);
	}
}

/* ------------------------------------------------------------------------ */
/* State, identity, hooks                                                    */
/* ------------------------------------------------------------------------ */

static const char *state_name(enum tedge_state s)
{
	static const char *const names[] = {
		"stopped",   "waiting-network", "waiting-time",
		"awaiting-registration", "connecting", "connected", "updating",
	};

	return ((size_t)s < ARRAY_SIZE(names)) ? names[s] : "?";
}

void tedge_set_state(enum tedge_state s)
{
	if ((enum tedge_state)atomic_set(&state, s) == s) {
		return;
	}
	LOG_INF("state: %s", state_name(s));
	if (hooks != NULL && hooks->on_state != NULL) {
		hooks->on_state(s, hooks->user_data);
	}
}

enum tedge_state tedge_get_state(void)
{
	return (enum tedge_state)atomic_get(&state);
}

const struct tedge_id *tedge_identity(void)
{
	return &identity;
}

const struct tedge_hooks *tedge_hook_table(void)
{
	return hooks;
}

static void progress(void)
{
	if (hooks != NULL && hooks->progress != NULL) {
		hooks->progress(hooks->user_data);
	}
}

/* ------------------------------------------------------------------------ */
/* Tenant host                                                               */
/* ------------------------------------------------------------------------ */

const char *tedge_c8y_host(void)
{
	return c8y_host[0] ? c8y_host : CONFIG_TEDGE_C8Y_URL;
}

int tedge_c8y_host_set(const char *host)
{
	if (host == NULL || host[0] == '\0' || strlen(host) >= sizeof(c8y_host)) {
		return -EINVAL;
	}
	strcpy(c8y_host, host);
	return settings_save_one(TEDGE_KEY_C8Y_URL, c8y_host, strlen(c8y_host));
}

int tedge_set_c8y_url(const char *host)
{
	if (!IS_ENABLED(CONFIG_TEDGE_TRANSPORT_C8Y)) {
		return -ENOTSUP;
	}
	return tedge_c8y_host_set(host);
}

static int settings_set_cb(const char *key, size_t len, settings_read_cb read_cb,
			   void *cb_arg)
{
	if (settings_name_steq(key, "c8y/url", NULL) && len < sizeof(c8y_host)) {
		ssize_t n = read_cb(cb_arg, c8y_host, sizeof(c8y_host) - 1);

		if (n > 0) {
			c8y_host[n] = '\0';
		}
		return 0;
	}
	return 0; /* other keys belong to the transport and onboarding */
}

SETTINGS_STATIC_HANDLER_DEFINE(tedge_core, TEDGE_SETTINGS_ROOT, NULL,
			       settings_set_cb, NULL, NULL);

/* ------------------------------------------------------------------------ */
/* Twin fragments                                                            */
/* ------------------------------------------------------------------------ */

int tedge_publish_twin(const char *fragment, const char *json)
{
	int rc = -ENOMEM;

	if (fragment == NULL || fragment[0] == '\0' ||
	    strlen(fragment) >= sizeof(twin[0].fragment)) {
		return -EINVAL;
	}
	k_mutex_lock(&twin_lock, K_FOREVER);
	for (int i = 0; i < TWIN_SLOTS; i++) {
		bool match = strcmp(twin[i].fragment, fragment) == 0;

		if (!match && twin[i].fragment[0] != '\0') {
			continue;
		}
		if (json == NULL) { /* forget it */
			if (match) {
				tedge_free(twin[i].json);
				twin[i].json = NULL;
				twin[i].fragment[0] = '\0';
			}
			rc = 0;
			break;
		}
		char *copy = tedge_alloc(strlen(json) + 1);

		if (copy == NULL) {
			break;
		}
		strcpy(copy, json);
		tedge_free(twin[i].json);
		twin[i].json = copy;
		strcpy(twin[i].fragment, fragment);
		twin_dirty = true;
		rc = 0;
		break;
	}
	k_mutex_unlock(&twin_lock);
	if (rc == 0) {
		k_event_post(&events, EV_WAKE);
	} else if (rc == -ENOMEM) {
		LOG_ERR("no free twin slot for \"%s\" (%d)", fragment, TWIN_SLOTS);
	}
	return rc;
}

void tedge_twin_republish(void)
{
	const struct tedge_transport *t = tedge_transport_get();

	k_mutex_lock(&twin_lock, K_FOREVER);
	for (int i = 0; i < TWIN_SLOTS; i++) {
		if (twin[i].fragment[0] != '\0' && twin[i].json != NULL) {
			(void)t->publish_twin(twin[i].fragment, twin[i].json);
		}
	}
	twin_dirty = false;
	k_mutex_unlock(&twin_lock);
}

/* ------------------------------------------------------------------------ */
/* Network events                                                            */
/* ------------------------------------------------------------------------ */

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED ||
	    event == NET_EVENT_IPV4_ADDR_ADD) { /* whichever the app provides */
		k_event_clear(&events, EV_NET_DOWN);
		k_event_post(&events, EV_NET_UP | EV_WAKE);
	} else if (event == NET_EVENT_L4_DISCONNECTED ||
		   event == NET_EVENT_IPV4_ADDR_DEL) {
		k_event_clear(&events, EV_NET_UP);
		k_event_post(&events, EV_NET_DOWN | EV_WAKE);
	}
}

static bool network_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface != NULL && net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED)) {
		return true;
	}
	return k_event_test(&events, EV_NET_UP) != 0;
}

/* Wait for connectivity. Returns false when the client is stopping. */
static bool wait_for_network(void)
{
	if (network_up()) {
		return true;
	}
	tedge_set_state(TEDGE_STATE_WAITING_NETWORK);
	while (!network_up()) {
		uint32_t got = k_event_wait(&events, EV_NET_UP | EV_STOP, false,
					    K_SECONDS(5));

		if (got & EV_STOP) {
			return false;
		}
		progress();
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/* Client thread                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t backoff_next(uint32_t current)
{
	uint32_t next = (current == 0) ? BACKOFF_MIN_S : current * 2;
	uint32_t max = CONFIG_TEDGE_RECONNECT_BACKOFF_MAX_S;

	if (next > max) {
		next = max;
	}
	/* ±20% jitter, so a fleet doesn't reconnect in lockstep. */
	int32_t spread = (int32_t)(next / 5);

	if (spread > 0) {
		next = (uint32_t)((int32_t)next - spread +
				  (int32_t)(sys_rand32_get() % (2 * spread + 1)));
	}
	return MAX(next, (uint32_t)BACKOFF_MIN_S);
}

static void client_thread(void *a, void *b, void *c)
{
	const struct tedge_transport *transport = tedge_transport_get();
	uint32_t backoff = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!(k_event_test(&events, EV_STOP))) {
		int64_t connected_at;
		int rc;

		if (!wait_for_network()) {
			break;
		}
		if (!tedge_time_is_valid()) {
			tedge_set_state(TEDGE_STATE_WAITING_TIME);
			if (tedge_time_sync() != 0) {
				k_sleep(K_SECONDS(5));
				continue;
			}
		}

		tedge_set_state(TEDGE_STATE_CONNECTING);
		rc = transport->connect();
		if (rc != 0) {
			backoff = backoff_next(backoff);
			LOG_WRN("connect failed (%d); retrying in %u s", rc,
				backoff);
			uint32_t got = k_event_wait(&events, EV_STOP | EV_NET_DOWN,
						    false, K_SECONDS(backoff));

			if (got & EV_STOP) {
				break;
			}
			continue;
		}

		connected_at = k_uptime_get();
		tedge_set_state(TEDGE_STATE_CONNECTED);
		tedge_twin_republish();

		/* Service the session until it ends. */
		while (!(k_event_test(&events, EV_STOP))) {
			if (k_event_test(&events, EV_NET_DOWN)) {
				rc = -ENETDOWN;
				break;
			}
			rc = transport->poll(1000);
			if (rc != 0) {
				break;
			}
			progress();
			if (twin_dirty) {
				tedge_twin_republish();
			}
		}
		transport->disconnect();
		if (k_uptime_get() - connected_at > STABLE_S * 1000) {
			backoff = 0; /* the session was healthy */
		}
		if (k_event_test(&events, EV_STOP)) {
			break;
		}
		LOG_WRN("session ended (%d)", rc);
		backoff = backoff_next(backoff);
		LOG_INF("reconnecting in %u s", backoff);
		(void)k_event_wait(&events, EV_STOP, false, K_SECONDS(backoff));
	}
	tedge_set_state(TEDGE_STATE_STOPPED);
}

static K_THREAD_STACK_DEFINE(client_stack, CONFIG_TEDGE_THREAD_STACK_SIZE);
static struct k_thread client_tid;
static bool running;

/* ------------------------------------------------------------------------ */
/* Public lifecycle                                                          */
/* ------------------------------------------------------------------------ */

static void copy_or_default(char *dst, size_t len, const char *value,
			    const char *fallback)
{
	snprintf(dst, len, "%s", (value != NULL && value[0]) ? value : fallback);
}

int tedge_init(const struct tedge_identity *id, const struct tedge_hooks *h)
{
	char mac_id[64] = "";

	if (initialised) {
		return -EALREADY;
	}
	hooks = h;

	(void)settings_subsys_init();
	(void)settings_load_subtree(TEDGE_SETTINGS_ROOT);

	if (tedge_platform_default_id(mac_id, sizeof(mac_id)) != 0) {
		snprintf(mac_id, sizeof(mac_id), "%s-unknown",
			 CONFIG_TEDGE_DEVICE_ID_PREFIX);
	}
	copy_or_default(identity.external_id, sizeof(identity.external_id),
			id ? id->external_id : NULL, mac_id);
	copy_or_default(identity.name, sizeof(identity.name),
			id ? id->name : NULL, identity.external_id);
	copy_or_default(identity.type, sizeof(identity.type),
			id ? id->type : NULL, CONFIG_TEDGE_DEVICE_TYPE);
	copy_or_default(identity.firmware_name, sizeof(identity.firmware_name),
			id ? id->firmware_name : NULL,
#if defined(CONFIG_APP_VERSION_EXTENDED_STRING) || defined(APP_VERSION_STRING)
			CONFIG_KERNEL_BIN_NAME
#else
			"zephyr"
#endif
	);
	copy_or_default(identity.firmware_version,
			sizeof(identity.firmware_version),
			id ? id->firmware_version : NULL,
#if defined(APP_VERSION_STRING)
			APP_VERSION_STRING
#else
			"0.0.0"
#endif
	);

	/* L4 events come from the connection manager when the application
	 * enables it; the IPv4 address events are always there. */
	net_mgmt_init_event_callback(&l4_cb, l4_handler,
				     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_ADDR_DEL |
					     (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)
						      ? NET_EVENT_L4_CONNECTED |
								NET_EVENT_L4_DISCONNECTED
						      : 0));
	net_mgmt_add_event_callback(&l4_cb);

	initialised = true;
	LOG_INF("tedge-zephyr %s: device \"%s\" (%s)", tedge_version(),
		identity.external_id, identity.type);
	return 0;
}

int tedge_start(void)
{
	if (!initialised) {
		return -EPERM;
	}
	if (running) {
		return -EALREADY;
	}
	k_event_clear(&events, EV_STOP);
	running = true;
	k_thread_create(&client_tid, client_stack,
			K_THREAD_STACK_SIZEOF(client_stack), client_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY),
			0, K_NO_WAIT);
	k_thread_name_set(&client_tid, "tedge");
	return 0;
}

int tedge_stop(void)
{
	if (!running) {
		return -EALREADY;
	}
	k_event_post(&events, EV_STOP | EV_WAKE);
	(void)k_thread_join(&client_tid, K_SECONDS(10));
	running = false;
	return 0;
}
