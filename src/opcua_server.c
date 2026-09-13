/* SPDX-License-Identifier: Apache-2.0 */

#include "opcua_server.h"
#include "address_space.h"
#include "net.h"
#include "display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <open62541.h>

LOG_MODULE_REGISTER(app_opcua, CONFIG_LOG_DEFAULT_LEVEL);

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

	/* Identify the application/server by the configured device name. */
	UA_LocalizedText_clear(&config->applicationDescription.applicationName);
	config->applicationDescription.applicationName =
		UA_LOCALIZEDTEXT_ALLOC("en-US", (char *)CONFIG_APP_DEVICE_NAME);

	/* Bound resources for a constrained device: cap sessions and channels
	 * so excess client connections are rejected rather than exhausting
	 * memory. Keep secure channels >= sessions (open62541 requirement). */
	config->maxSessions = 4;
	config->maxSecureChannels = 6;
	config->maxSessionTimeout = 60000.0; /* ms */

	/* Small per-connection send/recv buffers (8 kB) to fit constrained RAM.
	 * OPC-UA's minimum is 8192 bytes; our reads are tiny. */
	config->tcpBufSize = 8192;
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
	display_status_ipv4(DISPLAY_STAGE_SERVING, app_net_ipv4());

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
