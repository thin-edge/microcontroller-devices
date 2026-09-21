/* SPDX-License-Identifier: Apache-2.0
 *
 * Onboarding with the Cumulocity bootstrap user (the fallback when the device
 * has no certificate): connect to Core MQTT as the tenant's bootstrap user,
 * ask for device credentials on s/ucr until the operator accepts the device,
 * and store the "70,<tenant>,<user>,<password>" reply.
 *
 * It uses its own short-lived MQTT session so the main one always runs as the
 * device. Passwords are never logged.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#define BOOTSTRAP_PORT   8883
#define POLL_INTERVAL_MS 5000
#define WINDOW_MS        (2 * 60 * 1000)

static char dev_user[96];
static char dev_password[96];
static bool have_credentials;

/* Runtime credentials from tedge_set_bootstrap_credentials(). */
static char boot_user[96] = CONFIG_TEDGE_BOOTSTRAP_USER;
static char boot_password[96] = CONFIG_TEDGE_BOOTSTRAP_PASSWORD;

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[512];
static uint8_t tx_buf[512];
static uint8_t payload_buf[256];
static bool connack_seen;
static int connack_result;

int tedge_set_bootstrap_credentials(const char *user, const char *password)
{
	if (!IS_ENABLED(CONFIG_TEDGE_AUTH_BOOTSTRAP)) {
		return -ENOTSUP;
	}
	if (user == NULL || password == NULL || strlen(user) >= sizeof(boot_user) ||
	    strlen(password) >= sizeof(boot_password)) {
		return -EINVAL;
	}
	strcpy(boot_user, user);
	strcpy(boot_password, password);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

struct blob {
	char *buf;
	size_t cap;
	size_t len;
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct blob *b = param;

	ARG_UNUSED(key);
	if (len < b->cap) {
		b->len = read_cb(cb_arg, b->buf, len);
		b->buf[b->len] = '\0';
	}
	return 0;
}

static void load_stored(void)
{
	struct blob u = { .buf = dev_user, .cap = sizeof(dev_user) };
	struct blob p = { .buf = dev_password, .cap = sizeof(dev_password) };

	(void)settings_load_subtree_direct(TEDGE_KEY_BOOTSTRAP_USER, load_cb, &u);
	(void)settings_load_subtree_direct(TEDGE_KEY_BOOTSTRAP_PASS, load_cb, &p);
	have_credentials = (dev_user[0] != '\0' && dev_password[0] != '\0');
	if (have_credentials) {
		LOG_INF("device credentials found for \"%s\"", dev_user);
	}
}

/* ------------------------------------------------------------------------ */
/* The bootstrap session                                                     */
/* ------------------------------------------------------------------------ */

/* "70,<tenant>,<user>,<password>" */
static void parse_credentials(const char *line)
{
	char tenant[48], user[48], password[64];

	if (tedge_sr_template(line) != 70) {
		return;
	}
	if (tedge_sr_field(line, 1, tenant, sizeof(tenant)) < 0 ||
	    tedge_sr_field(line, 2, user, sizeof(user)) < 0 ||
	    tedge_sr_field(line, 3, password, sizeof(password)) < 0) {
		LOG_ERR("malformed device credentials");
		return;
	}
	snprintf(dev_user, sizeof(dev_user), "%s/%s", tenant, user);
	snprintf(dev_password, sizeof(dev_password), "%s", password);
	(void)settings_save_one(TEDGE_KEY_BOOTSTRAP_USER, dev_user,
				strlen(dev_user));
	(void)settings_save_one(TEDGE_KEY_BOOTSTRAP_PASS, dev_password,
				strlen(dev_password));
	memset(password, 0, sizeof(password));
	have_credentials = true;
	LOG_INF("device credentials received for \"%s\"", dev_user);
}

static void evt_cb(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connack_seen = true;
		connack_result = evt->result;
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		size_t len = p->message.payload.len;

		if (len >= sizeof(payload_buf)) {
			break;
		}
		if (mqtt_readall_publish_payload(c, payload_buf, len) == 0) {
			payload_buf[len] = '\0';
			parse_credentials((const char *)payload_buf);
		}
		break;
	}
	default:
		break;
	}
}

static int pump(int ms, const bool *done)
{
	int64_t end = k_uptime_get() + ms;

	do {
		struct zsock_pollfd fds = { .fd = client.transport.tls.sock,
					    .events = ZSOCK_POLLIN };
		int ret = zsock_poll(&fds, 1,
				     (int)CLAMP(end - k_uptime_get(), 0, 500));

		if (ret < 0) {
			return -errno;
		}
		if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
			ret = mqtt_input(&client);
			if (ret != 0) {
				return ret;
			}
		}
		ret = mqtt_live(&client);
		if (ret != 0 && ret != -EAGAIN) {
			return ret;
		}
	} while (k_uptime_get() < end && !(done && *done));
	return 0;
}

static int bootstrap_session(void)
{
	static const sec_tag_t tags[] = { TEDGE_TAG_SERVER_CA };
	struct zsock_addrinfo hints = { .ai_family = AF_INET,
					.ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	struct mqtt_utf8 user = { .utf8 = (const uint8_t *)boot_user,
				  .size = strlen(boot_user) };
	struct mqtt_utf8 pass = { .utf8 = (const uint8_t *)boot_password,
				  .size = strlen(boot_password) };
	struct mqtt_sec_config *tls = &client.transport.tls.config;
	struct mqtt_topic topic = {
		.topic.utf8 = (const uint8_t *)"s/dcr",
		.topic.size = 5,
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	struct mqtt_subscription_list sub = { .list = &topic,
					      .list_count = 1,
					      .message_id = 1 };
	int64_t deadline;
	char port[8];
	int ret;

	if (boot_user[0] == '\0' || boot_password[0] == '\0') {
		LOG_ERR("no bootstrap credentials: set CONFIG_TEDGE_BOOTSTRAP_USER "
			"and _PASSWORD, or call tedge_set_bootstrap_credentials()");
		return -EACCES;
	}

	snprintf(port, sizeof(port), "%d", BOOTSTRAP_PORT);
	ret = zsock_getaddrinfo(tedge_c8y_host(), port, &hints, &res);
	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = evt_cb;
	/* The client ID is the device's external ID, not the bootstrap user:
	 * that is how Cumulocity knows which registration is asking. */
	client.client_id.utf8 = (const uint8_t *)tedge_identity()->external_id;
	client.client_id.size = strlen(tedge_identity()->external_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.keepalive = 60;
	client.clean_session = 1;
	client.user_name = &user;
	client.password = &pass;
	client.transport.type = MQTT_TRANSPORT_SECURE;
	tls->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls->sec_tag_list = tags;
	tls->sec_tag_count = ARRAY_SIZE(tags);
	tls->hostname = tedge_c8y_host();

	connack_seen = false;
	ret = mqtt_connect(&client);
	if (ret != 0) {
		LOG_ERR("bootstrap connect failed (%d)", ret);
		return ret;
	}
	ret = pump(15000, &connack_seen);
	if (ret != 0 || !connack_seen || connack_result != 0) {
		LOG_ERR("no bootstrap CONNACK (ret=%d result=%d)", ret,
			connack_result);
		mqtt_abort(&client);
		return -ECONNREFUSED;
	}
	(void)mqtt_subscribe(&client, &sub);
	(void)pump(500, NULL);

	LOG_INF("waiting for the device registration to be accepted");
	deadline = k_uptime_get() + WINDOW_MS;
	while (!have_credentials && k_uptime_get() < deadline) {
		struct mqtt_publish_param p = {
			.message.topic.topic.utf8 = (const uint8_t *)"s/ucr",
			.message.topic.topic.size = 5,
			.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE,
			.message.payload.data = NULL,
			.message.payload.len = 0,
			.message_id = 0,
		};

		(void)mqtt_publish(&client, &p);
		ret = pump(POLL_INTERVAL_MS, &have_credentials);
		if (ret != 0) {
			break;
		}
	}
	(void)mqtt_disconnect(&client, NULL);
	k_msleep(300);
	return have_credentials ? 0 : -EAGAIN;
}

/* ------------------------------------------------------------------------ */
/* Interface used by the transport                                           */
/* ------------------------------------------------------------------------ */

int tedge_set_enroll_otp(const char *password)
{
	ARG_UNUSED(password);
	return -ENOTSUP; /* bootstrap devices have no one-time password */
}

int tedge_registration_url(char *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOTSUP; /* bootstrap devices are registered in the UI */
}

int tedge_auth_prepare(char *id_out, size_t id_len)
{
	snprintf(id_out, id_len, "%s", tedge_identity()->external_id);
	if (have_credentials) {
		return 0;
	}
	load_stored();
	if (have_credentials) {
		return 0;
	}
	tedge_set_state(TEDGE_STATE_AWAITING_REGISTRATION);
	return bootstrap_session();
}

const char *tedge_auth_username(void)
{
	return have_credentials ? dev_user : NULL;
}

const char *tedge_auth_password(void)
{
	return have_credentials ? dev_password : NULL;
}
