/* SPDX-License-Identifier: Apache-2.0
 *
 * The device agent on ESP-IDF: the thin-edge.io client (tiny-edge) on
 * FreeRTOS, with no protocol server. The device is managed from the cloud
 * (restart, firmware update, parameters, logs, remote access, shell) and
 * reports its own vitals as the "device" measurement.
 *
 * Set-up is over USB: tedge-onboard gives the device a Wi-Fi network, the
 * tenant, and a one-time password, and registers it in Cumulocity. Nothing
 * is built into the image, so one image serves every device and tenant.
 */

#include <tedge/provisioning_esp_idf.h>
#include <tedge/tedge.h>

#include <esp_app_desc.h>
#include <esp_console.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "agent";

/* "tedge-agent-idf-esp32c6", unless the build names it. */
static char firmware_name[48];

static void on_state(enum tedge_state state, void *user_data)
{
	(void)user_data;
	if (state == TEDGE_STATE_AWAITING_REGISTRATION) {
		char url[220];

		/* Only when the device was not set up with tedge-onboard. The
		 * URL carries a one-time password: it is for the person at the
		 * console. */
		if (tedge_registration_url(url, sizeof(url)) > 0) {
			ESP_LOGI(TAG, "register this device: %s", url);
		}
	}
}

/* Nothing on this device needs to finish first. */
static int restart_request(char *reason, size_t reason_len, void *user_data)
{
	(void)reason;
	(void)reason_len;
	(void)user_data;
	ESP_LOGI(TAG, "restart requested from the cloud");
	return 0;
}

static const struct tedge_hooks hooks = {
	.on_state = on_state,
	.restart_request = restart_request,
};

/* --- Wi-Fi --------------------------------------------------------------- */

/* Whether the station has a network to join: stored by tedge-onboard. */
static bool have_network(void)
{
	wifi_config_t cfg;

	return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
	       cfg.sta.ssid[0] != '\0';
}

/* Keeps the station connected: the client only watches the network. */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		if (have_network()) {
			(void)esp_wifi_connect();
		}
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *ev = data;

		/* Leaving on purpose (tedge-onboard switching networks, or
		 * scanning) is not a reason to join again. */
		if (ev->reason == WIFI_REASON_ASSOC_LEAVE || !have_network()) {
			return;
		}
		ESP_LOGW(TAG, "Wi-Fi: not connected (reason %d), trying again",
			 ev->reason);
		vTaskDelay(pdMS_TO_TICKS(1000));
		(void)esp_wifi_connect();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *ev = data;

		ESP_LOGI(TAG, "Wi-Fi: address " IPSTR, IP2STR(&ev->ip_info.ip));
	}
}

static void wifi_start(void)
{
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();

	esp_netif_create_default_wifi_sta();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						   on_wifi, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
						   on_wifi, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	if (have_network()) {
		ESP_LOGI(TAG, "Wi-Fi: using the network stored on the device");
	} else {
		ESP_LOGW(TAG, "no Wi-Fi network yet: set the device up with "
			 "tedge-onboard");
	}
	ESP_ERROR_CHECK(esp_wifi_start());
}

/* --- Commands the cloud's shell may run (c8y_Command) -------------------- */

static int cmd_free(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("heap: %u bytes free, %u at the lowest\n",
	       (unsigned int)esp_get_free_heap_size(),
	       (unsigned int)esp_get_minimum_free_heap_size());
	printf("largest free block: %u bytes\n",
	       (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	const esp_app_desc_t *app = esp_app_get_description();

	(void)argc;
	(void)argv;
	printf("%s %s, tiny-edge %s, ESP-IDF %s\n", app->project_name,
	       app->version, tedge_version(), app->idf_ver);
	return 0;
}

static int cmd_uptime(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("%lu s\n", (unsigned long)(xTaskGetTickCount() /
					  configTICK_RATE_HZ));
	return 0;
}

static void commands_register(void)
{
	static const esp_console_cmd_t cmds[] = {
		{ .command = "free", .help = "Free heap", .func = cmd_free },
		{ .command = "version", .help = "Firmware and library versions",
		  .func = cmd_version },
		{ .command = "uptime", .help = "Seconds since the start",
		  .func = cmd_uptime },
	};
	esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_console_init(&cfg));
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
	}
}

/* --- Telemetry ----------------------------------------------------------- */

/* The device's own vitals, as the Zephyr tedge-agent sends them: how long
 * it has been up, the free heap, and the signal it joined on. A value the
 * device cannot read is left out rather than sent as zero. */
static void publish_vitals(void)
{
	struct tedge_measurement_value values[3];
	wifi_ap_record_t ap;
	size_t count = 0;

	values[count++] = (struct tedge_measurement_value){
		.series = "uptime",
		.unit = "s",
		.value = (double)(xTaskGetTickCount() / configTICK_RATE_HZ),
	};
	values[count++] = (struct tedge_measurement_value){
		.series = "heap_free",
		.unit = "B",
		.value = (double)esp_get_free_heap_size(),
	};
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		values[count++] = (struct tedge_measurement_value){
			.series = "rssi",
			.unit = "dBm",
			.value = (double)ap.rssi,
		};
	}
	(void)tedge_publish_measurement(CONFIG_APP_MEASUREMENT_TYPE, values,
					count, 0);
}

/* ------------------------------------------------------------------------ */

void app_main(void)
{
	const esp_app_desc_t *app = esp_app_get_description();
	esp_err_t e = nvs_flash_init();

	if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
	    e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		/* A partition this firmware cannot read: start it afresh. */
		ESP_ERROR_CHECK(nvs_flash_erase());
		e = nvs_flash_init();
	}
	ESP_ERROR_CHECK(e);
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_start();
	commands_register();

	if (CONFIG_APP_FIRMWARE_NAME[0] != '\0') {
		snprintf(firmware_name, sizeof(firmware_name), "%s",
			 CONFIG_APP_FIRMWARE_NAME);
	} else {
		snprintf(firmware_name, sizeof(firmware_name), "%s-%s",
			 app->project_name, CONFIG_IDF_TARGET);
	}
	ESP_LOGI(TAG, "%s %s, tiny-edge %s", firmware_name, app->version,
		 tedge_version());

	const struct tedge_identity id = {
		.type = CONFIG_APP_DEVICE_TYPE,
		.firmware_name = firmware_name,
	};

	if (tedge_init(&id, &hooks) != 0) {
		ESP_LOGE(TAG, "the client did not start");
		return;
	}
	if (tedge_provisioning_esp_idf_init(NULL) != 0) {
		ESP_LOGE(TAG, "tedge-onboard support did not start");
	}
	if (tedge_start() != 0) {
		ESP_LOGE(TAG, "the client did not start");
		return;
	}

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_MEASUREMENT_INTERVAL_S *
					 1000));
		publish_vitals();
	}
}
