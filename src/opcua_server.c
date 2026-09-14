/* SPDX-License-Identifier: Apache-2.0 */

#include "opcua_server.h"
#include "address_space.h"
#include "net.h"
#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <open62541.h>

LOG_MODULE_REGISTER(app_opcua, CONFIG_LOG_DEFAULT_LEVEL);

/* Called by the vendored open62541 event loop (Zephyr patch #9) when select()
 * fails — e.g. a transient ENOMEM from the socket layer under connection churn.
 * Yield briefly so the retry does not spin at ~100 Hz and starve the Wi-Fi/net
 * threads (which otherwise makes the whole device fall off the network). */
void ua_zephyr_backoff(void)
{
	k_msleep(50);
}

/* The open62541 encoders/decoders are stack-hungry; give the server thread
 * plenty of headroom. */
#define OPCUA_THREAD_STACK_SIZE 16384
#define OPCUA_THREAD_PRIORITY   5

K_THREAD_STACK_DEFINE(opcua_stack, OPCUA_THREAD_STACK_SIZE);
static struct k_thread opcua_thread;

static UA_Server *server;
static volatile bool running;

static void configure_server(UA_Server *srv)
{
	UA_ServerConfig *config = UA_Server_getConfig(srv);

	/* Identify the application/server by the unique per-device hostname so
	 * multiple devices on the network are individually distinguishable. */
	UA_LocalizedText_clear(&config->applicationDescription.applicationName);
	config->applicationDescription.applicationName =
		UA_LOCALIZEDTEXT_ALLOC("en-US", (char *)app_net_hostname());

	/* Bound resources for a constrained device: cap sessions and channels
	 * so excess client connections are rejected rather than exhausting
	 * memory. Keep secure channels >= sessions (open62541 requirement). */
	config->maxSessions = 4;
	config->maxSecureChannels = 6;
	/* Reclaim abandoned sessions quickly. A client that disconnects without a
	 * clean CloseSession (common with a polling collector or after a Wi-Fi
	 * blip) otherwise pins session resources for the full timeout; under
	 * repeated reconnects that starves the device. 10 s is ample for a healthy
	 * client to stay alive with keep-alives. */
	config->maxSessionTimeout = 10000.0; /* ms */

	/* Small per-connection send/recv buffers (8 kB) to fit constrained RAM.
	 * OPC-UA's minimum is 8192 bytes; our reads are tiny. */
	config->tcpBufSize = 8192;

#ifdef UA_ENABLE_SUBSCRIPTIONS
	/* Cap subscription resources so a client cannot exhaust device RAM. */
	config->maxSubscriptions = CONFIG_APP_OPCUA_MAX_SUBSCRIPTIONS;
	config->maxMonitoredItemsPerSubscription =
		CONFIG_APP_OPCUA_MAX_MONITORED_ITEMS;
#endif
}

static void opcua_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	UA_StatusCode rc;
	static UA_ServerConfig config;

	memset(&config, 0, sizeof(config));
	rc = UA_ServerConfig_setMinimal(&config, CONFIG_APP_OPCUA_PORT, NULL);
	if (rc != UA_STATUSCODE_GOOD) {
		LOG_ERR("Server config failed: %s (0x%08x)",
			UA_StatusCode_name(rc), rc);
		return;
	}

	server = UA_Server_newWithConfig(&config);
	if (server == NULL) {
		LOG_ERR("UA_Server_newWithConfig() failed");
		return;
	}

	configure_server(server);

	rc = address_space_setup(server);
	if (rc != UA_STATUSCODE_GOOD) {
		goto cleanup;
	}

	rc = UA_Server_run_startup(server);
	if (rc != UA_STATUSCODE_GOOD) {
		LOG_ERR("Server startup failed: %s", UA_StatusCode_name(rc));
		goto cleanup;
	}

	LOG_INF("OPC-UA server listening on opc.tcp://<device>:%d",
		CONFIG_APP_OPCUA_PORT);
	app_net_show_status(DISPLAY_STAGE_SERVING);

	/* Drive the server loop and sampling ourselves. We poll rather than let
	 * open62541 block in select() internally (more robust across Zephyr's
	 * socket backends), and we sample from the data source on Zephyr's
	 * monotonic clock (k_uptime_get) rather than open62541's repeated
	 * callbacks, whose POSIX timer clock does not advance under Zephyr. */
	int64_t last_sample = 0;
	while (running) {
		UA_Server_run_iterate(server, false);

		int64_t now = k_uptime_get();
		if (now - last_sample >= CONFIG_APP_SAMPLE_INTERVAL_MS) {
			address_space_update(server);
			last_sample = now;
		}
		k_msleep(10);
	}

	UA_Server_run_shutdown(server);

cleanup:
	UA_Server_delete(server);
	server = NULL;
}

int opcua_server_start(void)
{
	if (running) {
		return -EALREADY;
	}
	running = true;

	k_thread_create(&opcua_thread, opcua_stack,
			K_THREAD_STACK_SIZEOF(opcua_stack),
			opcua_thread_fn, NULL, NULL, NULL,
			OPCUA_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&opcua_thread, "opcua");
	return 0;
}

void opcua_server_stop(void)
{
	running = false;
	k_thread_join(&opcua_thread, K_SECONDS(5));
}
