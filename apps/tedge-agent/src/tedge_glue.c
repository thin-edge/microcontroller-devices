/* SPDX-License-Identifier: Apache-2.0
 *
 * The glue between this application and the tedge-zephyr client. It lives
 * here, not in the module: the module knows nothing about lib/common.
 *
 * This application has no protocol of its own, so the glue is most of it:
 * the device's identity, the registration URL while it waits to be
 * registered, the status LED driven from the client's state, the platform
 * reset this repository uses (a CPU reset hangs MCUboot on the ESP32-C6),
 * and a little telemetry about the device itself so that a bare agent still
 * shows something in the cloud.
 *
 * Remote access follows the client's Kconfig policy (hosts on the device's
 * own LAN by default), so there is no allow hook here.
 */

#include "tedge_glue.h"

#include "boot_request.h"
#include "identity.h"
#include "net.h"
#if defined(CONFIG_APP_WIFI_CRED_STORE)
#include "prov_c8y.h"
#endif
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_WIFI)
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#endif
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
#include <zephyr/sys/sys_heap.h>
#endif

#include <tedge/tedge.h>

LOG_MODULE_REGISTER(tedge_glue, CONFIG_LOG_DEFAULT_LEVEL);

static void on_state(enum tedge_state state, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (state) {
	case TEDGE_STATE_AWAITING_REGISTRATION: {
		char url[220];

		/* The operator registers the device from this URL. It carries
		 * the one-time password, so the module never logs it itself. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			LOG_INF("register this device: %s", url);
		}
		status_led_set_mode(STATUS_LED_PROVISIONING);
		break;
	}
	case TEDGE_STATE_CONNECTED:
		status_led_set_mode(STATUS_LED_CONNECTED);
		break;
	default:
		break;
	}
}

/* Nothing on this device needs to finish first. */
static int restart_request(char *reason, size_t reason_len, void *user_data)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(reason_len);
	ARG_UNUSED(user_data);
	LOG_INF("restart requested from the cloud");
	return 0;
}

static void reset(void *user_data)
{
	ARG_UNUSED(user_data);
	boot_request_reboot();
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
	.restart_request = restart_request,
	.reset = reset,
};

#if defined(CONFIG_TEDGE_TELEMETRY)
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct k_heap _system_heap;
#endif

/* The device's own vitals: how long it has been up, how much kernel heap
 * the Wi-Fi driver and the client leave free, and the signal it joined on.
 * Values the device cannot read are left out rather than sent as zero. */
static void telemetry_fn(struct k_work *work)
{
	struct tedge_measurement_value values[3];
	size_t count = 0;

	values[count++] = (struct tedge_measurement_value){
		.series = "uptime",
		.unit = "s",
		.value = (double)(k_uptime_get() / 1000),
	};
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats heap;

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &heap) == 0) {
		values[count++] = (struct tedge_measurement_value){
			.series = "heap_free",
			.unit = "B",
			.value = (double)heap.free_bytes,
		};
	}
#endif
#if defined(CONFIG_WIFI)
	struct net_if *wifi = net_if_get_first_wifi();
	struct wifi_iface_status st = {0};

	/* Only while associated: a status query against a driver that is
	 * reconnecting can block. */
	if (wifi != NULL && app_net_is_connected() &&
	    net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi, &st, sizeof(st)) ==
		    0) {
		values[count++] = (struct tedge_measurement_value){
			.series = "rssi",
			.unit = "dBm",
			.value = (double)st.rssi,
		};
	}
#endif
	(void)tedge_publish_measurement(CONFIG_APP_TEDGE_MEASUREMENT_TYPE,
					values, count, 0);
	(void)k_work_reschedule(k_work_delayable_from_work(work),
				K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
}

static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);
#endif /* CONFIG_TEDGE_TELEMETRY */

int tedge_glue_start(void)
{
	struct tedge_identity id = {
		.external_id = app_identity_device_id(),
		.name = app_identity_device_id(),
		.type = "thin-edge.io-zephyr-agent",
		.firmware_name = app_identity_firmware_name(),
		.firmware_version = app_identity_firmware_version(),
	};
	int rc = tedge_init(&id, &hooks);

	if (rc != 0) {
		LOG_ERR("tedge_init failed (%d)", rc);
		return rc;
	}
#if defined(CONFIG_APP_WIFI_CRED_STORE)
	/* Tenant and one-time password from the ZTP provisioner, if it left
	 * any. The external ID already came through app_identity_device_id(). */
	(void)prov_c8y_handoff();
#endif
	rc = tedge_start();
	if (rc != 0) {
		LOG_ERR("tedge_start failed (%d)", rc);
		return rc;
	}
#if defined(CONFIG_TEDGE_TELEMETRY)
	(void)k_work_schedule(&telemetry_work,
			      K_SECONDS(CONFIG_APP_TEDGE_MEASUREMENT_INTERVAL_S));
#endif
	return 0;
}
