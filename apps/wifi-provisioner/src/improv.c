/* SPDX-License-Identifier: Apache-2.0
 *
 * Improv Wi-Fi over BLE (https://www.improv-wifi.com/ble/): the GATT service
 * and the provisioning state machine of the Wi-Fi provisioner. Credentials are
 * only stored after the device has joined the network with them. Every exit
 * clears the boot request and reboots, so MCUboot runs the application next.
 */

#include "improv.h"
#include "boot_request.h"
#include "net.h"
#include "prov_identity.h"
#include "status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_credentials.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(improv, CONFIG_LOG_DEFAULT_LEVEL);

#define AUTH_PERIOD_MS     60000 /* Improv convention: 1 minute */
#define IDENTIFY_MS        10000
#define RESULT_GRACE_MS    5000  /* time for the client to read the result */
#define BTN_DEBOUNCE_MS    30

/* ------------------------------------------------------------------------- */
/* Provisioning-mode events                                                  */
/* ------------------------------------------------------------------------- */

enum prov_msg_type {
	MSG_SETTINGS, /* Improv "send Wi-Fi settings" RPC */
	MSG_BUTTON,   /* sw0 pressed */
};

struct prov_msg {
	enum prov_msg_type type;
	struct app_wifi_creds creds;
};

K_MSGQ_DEFINE(prov_q, sizeof(struct prov_msg), 2, 4);

/* ------------------------------------------------------------------------- */
/* sw0: a short press restarts an expired window or authorizes              */
/* ------------------------------------------------------------------------- */

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback btn_cb;
static struct k_work_delayable btn_work;
static bool btn_level;

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	(void)k_work_reschedule_for_queue(app_net_workq(), &btn_work,
					  K_MSEC(BTN_DEBOUNCE_MS));
}

static void btn_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	bool level = gpio_pin_get_dt(&btn) > 0;

	if (level != btn_level) {
		btn_level = level;
		if (level) {
			struct prov_msg m = { .type = MSG_BUTTON };

			(void)k_msgq_put(&prov_q, &m, K_NO_WAIT);
		}
	}
}

static void button_start(void)
{
	if (!gpio_is_ready_dt(&btn)) {
		return;
	}
	k_work_init_delayable(&btn_work, btn_work_fn);
	if (gpio_pin_configure_dt(&btn, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH) != 0) {
		LOG_ERR("sw0 configuration failed");
		return;
	}
	btn_level = gpio_pin_get_dt(&btn) > 0;
	gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
	(void)gpio_add_callback(btn.port, &btn_cb);
}

#else /* no sw0 on this board */

static void button_start(void)
{
}

#endif

/* ------------------------------------------------------------------------- */
/* Improv Wi-Fi GATT service                                                 */
/* ------------------------------------------------------------------------- */

#define IMPROV_UUID(n) \
	BT_UUID_128_ENCODE(0x00467768, 0x6228, 0x2272, 0x4663, 0x277478268000 + (n))

enum improv_state {
	ST_AUTH_REQUIRED = 0x01,
	ST_AUTHORIZED = 0x02,
	ST_PROVISIONING = 0x03,
	ST_PROVISIONED = 0x04,
};

enum improv_error {
	ERR_NONE = 0x00,
	ERR_INVALID_RPC = 0x01,
	ERR_UNKNOWN_RPC = 0x02,
	ERR_UNABLE_TO_CONNECT = 0x03,
	ERR_NOT_AUTHORIZED = 0x04,
	ERR_UNKNOWN = 0xFF,
};

enum improv_cmd {
	CMD_WIFI_SETTINGS = 0x01,
	CMD_IDENTIFY = 0x02,
};

#define CAP_IDENTIFY 0x01
#define FRAME_MAX    (3 + 1 + 32 + 1 + 64) /* cmd, len, csum + settings */

static const struct bt_uuid_128 uuid_svc = BT_UUID_INIT_128(IMPROV_UUID(0));
static const struct bt_uuid_128 uuid_state = BT_UUID_INIT_128(IMPROV_UUID(1));
static const struct bt_uuid_128 uuid_error = BT_UUID_INIT_128(IMPROV_UUID(2));
static const struct bt_uuid_128 uuid_rpc = BT_UUID_INIT_128(IMPROV_UUID(3));
static const struct bt_uuid_128 uuid_result = BT_UUID_INIT_128(IMPROV_UUID(4));
static const struct bt_uuid_128 uuid_caps = BT_UUID_INIT_128(IMPROV_UUID(5));

static uint8_t cur_state = ST_AUTHORIZED;
static uint8_t cur_error = ERR_NONE;
static uint8_t caps;
static uint8_t result_buf[FRAME_MAX + 64];
static uint16_t result_len;

static uint8_t rx_buf[FRAME_MAX];
static uint16_t rx_len;
static int64_t rx_last;

static ssize_t read_u8(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(uint8_t));
}

static ssize_t read_result(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, result_buf,
				 result_len);
}

static ssize_t write_rpc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags);

BT_GATT_SERVICE_DEFINE(improv_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),
	BT_GATT_CHARACTERISTIC(&uuid_state.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_u8, NULL, &cur_state),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_error.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_u8, NULL, &cur_error),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_rpc.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE,
			       NULL, write_rpc, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_result.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_result, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_caps.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_u8, NULL, &caps),
);

/* --- Advertising: service UUID + Improv service data; name in scan resp --- */

static uint8_t svc_data[8] = { 0x77, 0x46 }; /* UUID 0x4677, LE */
static const uint8_t ad_uuid[] = { IMPROV_UUID(0) };
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_UUID128_ALL, ad_uuid, sizeof(ad_uuid)),
	BT_DATA(BT_DATA_SVC_DATA16, svc_data, sizeof(svc_data)),
};
static struct bt_data sd[1];

static bool adv_wanted;
static bool adv_running;
static struct bt_conn *cur_conn;
static K_SEM_DEFINE(disconnected_sem, 0, 1);

static void adv_refresh_data(void)
{
	svc_data[2] = cur_state;
	svc_data[3] = caps;
	if (adv_running) {
		(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	}
}

static void adv_start(void)
{
	if (!adv_wanted || adv_running || cur_conn != NULL) {
		return;
	}
	adv_refresh_data();
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}
	adv_running = true;
}

static void adv_stop(void)
{
	adv_wanted = false;
	if (adv_running) {
		(void)bt_le_adv_stop();
		adv_running = false;
	}
}

static void adv_restart_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	adv_start();
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_fn);

/* The credential test runs Wi-Fi association and DHCP while the BLE link is
 * up, and with software coexistence on a single radio the link can miss
 * events for a few seconds. With a central's usual ~2 s supervision timeout
 * the link then drops and the client never sees the result, so ask for 5 s.
 * The values stay within Apple's accessory limits (interval 30-60 ms, no
 * latency, timeout 2-6 s) so macOS and iOS centrals accept them. */
static const struct bt_le_conn_param conn_param =
	BT_LE_CONN_PARAM_INIT(24, 48, 0, 500);

static void conn_param_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (cur_conn != NULL) {
		int err = bt_conn_le_param_update(cur_conn, &conn_param);

		if (err) {
			LOG_WRN("Connection parameter request failed (%d)", err);
		}
	}
}

static K_WORK_DELAYABLE_DEFINE(conn_param_work, conn_param_fn);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	adv_running = false; /* connectable advertising stops on connect */
	cur_conn = bt_conn_ref(conn);
	LOG_INF("Provisioning client connected");
	/* After service discovery has settled. */
	k_work_reschedule(&conn_param_work, K_MSEC(500));
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
				uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("Connection parameters: interval %u.%02u ms, latency %u, "
		"timeout %u ms", interval * 5 / 4, (interval * 125) % 100,
		latency, timeout * 10);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Provisioning client disconnected (0x%02x)", reason);
	if (cur_conn == conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	rx_len = 0;
	k_sem_give(&disconnected_sem);
}

static void on_recycled(void)
{
	k_work_submit(&adv_restart_work);
}

BT_CONN_CB_DEFINE(prov_conn_cbs) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_updated = on_le_param_updated,
	.recycled = on_recycled,
};

static void notify_u8(const struct bt_uuid *uuid, uint8_t v)
{
	const struct bt_gatt_attr *a =
		bt_gatt_find_by_uuid(improv_svc.attrs, improv_svc.attr_count, uuid);

	if (a != NULL && cur_conn != NULL) {
		(void)bt_gatt_notify(cur_conn, a, &v, sizeof(v));
	}
}

static void set_state(uint8_t s)
{
	cur_state = s;
	notify_u8(&uuid_state.uuid, s);
	adv_refresh_data();
}

static void set_error(uint8_t e)
{
	cur_error = e;
	notify_u8(&uuid_error.uuid, e);
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < n; i++) {
		sum += p[i];
	}
	return (uint8_t)sum;
}

/* Validate and dispatch one complete RPC frame (BT RX context). */
static void handle_frame(const uint8_t *f, size_t n)
{
	uint8_t cmd = f[0];
	uint8_t len = f[1];
	const uint8_t *d = &f[2];

	if (n != (size_t)len + 3 || checksum(f, n - 1) != f[n - 1]) {
		LOG_WRN("Improv RPC rejected: bad length or checksum");
		set_error(ERR_INVALID_RPC);
		return;
	}

	switch (cmd) {
	case CMD_IDENTIFY:
		set_error(ERR_NONE);
		status_led_flash(STATUS_LED_IDENTIFY, IDENTIFY_MS);
		return;
	case CMD_WIFI_SETTINGS: {
		struct prov_msg m = { .type = MSG_SETTINGS };
		uint8_t ssid_len = len >= 1 ? d[0] : 0;
		uint8_t psk_len;

		if (len < 2 || ssid_len == 0 || ssid_len > 32 ||
		    (size_t)ssid_len + 2 > len) {
			set_error(ERR_INVALID_RPC);
			return;
		}
		psk_len = d[1 + ssid_len];
		if (psk_len > 64 || (size_t)ssid_len + psk_len + 2 != len) {
			set_error(ERR_INVALID_RPC);
			return;
		}
		if (cur_state == ST_AUTH_REQUIRED) {
			set_error(ERR_NOT_AUTHORIZED);
			return;
		}
		if (cur_state != ST_AUTHORIZED) {
			return; /* already testing or provisioned */
		}
		memcpy(m.creds.ssid, &d[1], ssid_len);
		m.creds.ssid_len = ssid_len;
		memcpy(m.creds.psk, &d[2 + ssid_len], psk_len);
		m.creds.psk_len = psk_len;
		set_error(ERR_NONE);
		/* Move to "provisioning" now so a repeated write is ignored. */
		set_state(ST_PROVISIONING);
		if (k_msgq_put(&prov_q, &m, K_NO_WAIT) != 0) {
			set_state(ST_AUTHORIZED);
			set_error(ERR_UNKNOWN);
		}
		memset(&m, 0, sizeof(m));
		return;
	}
	default:
		set_error(ERR_UNKNOWN_RPC);
		return;
	}
}

static ssize_t write_rpc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return 0; /* queued write: the data arrives on execute */
	}

	int64_t now = k_uptime_get();

	if (offset == 0) {
		/* A new frame, unless this continues a frame that an earlier
		 * write left incomplete (a frame split across writes). */
		bool continuing = rx_len >= 2 && rx_len < rx_buf[1] + 3u &&
				  now - rx_last < 2000 &&
				  !(flags & BT_GATT_WRITE_FLAG_EXECUTE);

		if (!continuing) {
			rx_len = 0;
		}
	} else if (offset != rx_len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	rx_last = now;

	if ((size_t)rx_len + len > sizeof(rx_buf)) {
		rx_len = 0;
		set_error(ERR_INVALID_RPC);
		return len;
	}
	memcpy(&rx_buf[rx_len], buf, len);
	rx_len += len;

	if (rx_len >= 2 && rx_len >= rx_buf[1] + 3u) {
		handle_frame(rx_buf, rx_len);
		memset(rx_buf, 0, sizeof(rx_buf));
		rx_len = 0;
	}
	return len;
}

/* ------------------------------------------------------------------------- */
/* Provisioning state machine                                                */
/* ------------------------------------------------------------------------- */

/* The application this provisioner hands back to (from its identity record);
 * has_ident is false before any application has run. */
static struct prov_identity ident;
static bool has_ident;

/* URL scheme for the RPC result, from the DNS-SD service type. */
static const char *url_scheme(const char *t)
{
	if (strcmp(t, "_opcua-tcp") == 0) {
		return "opc.tcp";
	}
	return (t[0] == '_') ? t + 1 : t;
}

/* Fill and notify the RPC result: the application's service URL, or an empty
 * URL list when no application identity is known yet. */
static void set_result_url(void)
{
	char url[80];
	int n = 0;

	if (has_ident) {
		n = snprintf(url, sizeof(url), "%s://%s.local:%u",
			     url_scheme(ident.service), ident.hostname,
			     ident.port);
		if (n < 0 || n >= (int)sizeof(url)) {
			n = 0;
		}
	}
	/* [cmd][len][str_len][str][checksum]; no strings when n is 0 */
	result_buf[0] = CMD_WIFI_SETTINGS;
	if (n > 0) {
		result_buf[1] = (uint8_t)(n + 1);
		result_buf[2] = (uint8_t)n;
		memcpy(&result_buf[3], url, n);
		result_len = 3 + n;
	} else {
		result_buf[1] = 0;
		result_len = 2;
	}
	result_buf[result_len] = checksum(result_buf, result_len);
	result_len++;

	const struct bt_gatt_attr *a = bt_gatt_find_by_uuid(
		improv_svc.attrs, improv_svc.attr_count, &uuid_result.uuid);

	if (a != NULL && cur_conn != NULL) {
		(void)bt_gatt_notify(cur_conn, a, result_buf, result_len);
	}
	LOG_INF("Provisioned%s%s", n ? ": " : "", n ? url : "");
}

static uint8_t idle_state(void)
{
	return IS_ENABLED(CONFIG_APP_WIFI_PROV_REQUIRE_AUTH) ? ST_AUTH_REQUIRED
							     : ST_AUTHORIZED;
}

/* Every exit: clear the boot request so MCUboot runs the application. */
static FUNC_NORETURN void reboot_now(void)
{
	int rc = boot_request_clear();

	if (rc != 0) {
		LOG_ERR("Could not clear the boot request (%d)", rc);
	}
	LOG_INF("Leaving the provisioner: rebooting into the application");
	log_flush(); /* deferred logging: print before the reset */
	boot_request_reboot();
	CODE_UNREACHABLE;
}

/* Test the credentials; on success store them and reboot. */
static void try_settings(struct app_wifi_creds *c, int64_t auth_until)
{
	LOG_INF("Testing credentials for SSID \"%s\"", c->ssid);
	int rc = app_net_try_credentials(
		c, K_SECONDS(CONFIG_APP_WIFI_PROV_CONNECT_TIMEOUT_S));

	if (rc != 0) {
		LOG_WRN("Could not join \"%s\" (%d); nothing stored", c->ssid, rc);
		set_error(ERR_UNABLE_TO_CONNECT);
		set_state((auth_until && k_uptime_get() < auth_until) ||
				  !IS_ENABLED(CONFIG_APP_WIFI_PROV_REQUIRE_AUTH)
			  ? ST_AUTHORIZED : ST_AUTH_REQUIRED);
		return;
	}

	(void)wifi_credentials_delete_all();
	rc = wifi_credentials_set_personal(
		c->ssid, c->ssid_len,
		c->psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		NULL, 0, c->psk, c->psk_len, 0, 0, 0);
	if (rc != 0) {
		LOG_ERR("Storing the credentials failed (%d)", rc);
		set_error(ERR_UNKNOWN);
		set_state(idle_state());
		return;
	}

	adv_stop();
	set_state(ST_PROVISIONED);
	set_result_url();

	/* Give the client time to read the result, then start serving. */
	k_sem_reset(&disconnected_sem);
	if (cur_conn != NULL) {
		(void)k_sem_take(&disconnected_sem, K_MSEC(RESULT_GRACE_MS));
	}
	reboot_now();
}

FUNC_NORETURN void improv_run(void)
{
	char name[CONFIG_BT_DEVICE_NAME_MAX];
	int64_t deadline = k_uptime_get() + CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
	int64_t auth_until = 0;
	bool idle = false;
	struct boot_request req;
	/* Entered with the button pattern: the application still has working
	 * credentials, so the window's end returns to it. */
	bool return_on_expiry = boot_request_read(&req) == 0 &&
				req.reason == BOOT_REQUEST_OPERATOR;

	status_led_set_mode(STATUS_LED_PROVISIONING);
	button_start();

	caps = status_led_present() ? CAP_IDENTIFY : 0;
	cur_state = idle_state();
	has_ident = prov_identity_load(&ident) == 0;

	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		k_sleep(K_SECONDS(30));
		boot_request_reboot(); /* retry; the request stays set */
	}

	/* The BLE name is capped at CONFIG_BT_DEVICE_NAME_MAX - 1 characters. */
	snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1,
		 has_ident ? ident.hostname : app_net_hostname());
	(void)bt_set_name(name);
	sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, bt_get_name(),
					strlen(bt_get_name()));

	adv_wanted = true;
	adv_start();
	LOG_INF("Advertising Improv Wi-Fi as \"%s\" for %d s", name,
		CONFIG_APP_WIFI_PROV_WINDOW_S);

	for (;;) {
		int64_t now = k_uptime_get();
		int64_t wake = idle ? INT64_MAX : deadline;

		if (auth_until && auth_until < wake) {
			wake = auth_until;
		}

		struct prov_msg m;
		k_timeout_t to = (wake == INT64_MAX) ? K_FOREVER
			       : K_MSEC(MAX(wake - now, 0));

		if (k_msgq_get(&prov_q, &m, to) == 0) {
			if (m.type == MSG_SETTINGS && !idle) {
				try_settings(&m.creds, auth_until);
				memset(&m, 0, sizeof(m));
			} else if (m.type == MSG_BUTTON) {
				if (idle) {
					LOG_INF("Button: advertising again");
					idle = false;
					deadline = k_uptime_get() +
						   CONFIG_APP_WIFI_PROV_WINDOW_S * 1000LL;
					status_led_set_mode(STATUS_LED_PROVISIONING);
					adv_wanted = true;
					adv_start();
				} else if (cur_state == ST_AUTH_REQUIRED ||
					   (cur_state == ST_AUTHORIZED &&
					    IS_ENABLED(CONFIG_APP_WIFI_PROV_REQUIRE_AUTH))) {
					LOG_INF("Button: authorized for %d s",
						AUTH_PERIOD_MS / 1000);
					auth_until = k_uptime_get() + AUTH_PERIOD_MS;
					set_state(ST_AUTHORIZED);
				}
			}
			continue;
		}

		now = k_uptime_get();
		if (auth_until && now >= auth_until) {
			auth_until = 0;
			if (cur_state == ST_AUTHORIZED) {
				set_state(ST_AUTH_REQUIRED);
			}
		}
		if (!idle && now >= deadline) {
			struct app_wifi_creds c;
			bool have = return_on_expiry ||
				    app_wifi_creds_resolve(&c) == 0;

			memset(&c, 0, sizeof(c));
			if (have) {
				LOG_INF("Provisioning window expired; returning "
					"to the configured network");
				reboot_now();
			}
			LOG_INF("Provisioning window expired; idle until "
				"the button is pressed");
			adv_stop();
			if (cur_conn != NULL) {
				(void)bt_conn_disconnect(
					cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			}
			status_led_set_mode(STATUS_LED_OFF);
			idle = true;
		}
	}
}
