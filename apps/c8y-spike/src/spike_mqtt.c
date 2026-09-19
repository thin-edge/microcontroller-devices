/* SPDX-License-Identifier: Apache-2.0
 *
 * Spike A (c8y-direct-spikes, tasks 3.2-3.8): MQTTS from the device straight
 * to Cumulocity, measured. Throwaway code: written to answer U1-U4, not to be
 * kept. The production client goes into tedge-zephyr.
 *
 * What it does, on its own thread (the TLS handshake needs a deep stack):
 * - optional connect/disconnect cycles, timing each handshake (3.5);
 * - connect with mutual TLS (or basic auth on Core MQTT), subscribe one
 *   filter per SUBSCRIBE (the MQTT Service refuses several per packet);
 * - publish 100/114/117, request a JWT on s/uat;
 * - handle 510 (restart): 501, persist a marker, reboot, 503 after reconnect;
 * - publish telemetry from the data model every CONFIG_SPIKE_TELEMETRY_INTERVAL_S
 *   (free-form on the MQTT Service, SmartREST 200 on Core MQTT);
 * - log the mbedTLS heap (current and peak) and the thread's stack headroom.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>

#include <mbedtls/memory_buffer_alloc.h>

#if defined(CONFIG_SPIKE_OTA)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "boot_request.h"
#include "data_source.h"
#include "spike.h"

LOG_MODULE_REGISTER(spike_mqtt, LOG_LEVEL_INF);

#define TAG_SERVER_CA SPIKE_TAG_SERVER_CA
#define TAG_DEVICE    SPIKE_TAG_DEVICE

/* The external ID: from Kconfig, or from enrollment (Spike C). */
static char device_id[48] = CONFIG_SPIKE_DEVICE_ID;
#define DEVICE_ID device_id

static const unsigned char server_ca[] = {
#include "spike/server_ca.pem.inc"
	0x00};

#if defined(CONFIG_SPIKE_AUTH_CERT) || defined(CONFIG_SPIKE_AUTH_ENROLLED)
static const sec_tag_t sec_tags[] = {TAG_SERVER_CA, TAG_DEVICE};
#else
static const sec_tag_t sec_tags[] = {TAG_SERVER_CA};
#endif

#if defined(CONFIG_SPIKE_AUTH_CERT)
static const unsigned char device_crt[] = {
#include "spike/device.crt.inc"
	0x00};
static const unsigned char device_key[] = {
#include "spike/device.key.inc"
	0x00};
#endif

/* Downstream topics: tedge's bridge set minus s/ucr (bootstrap only). */
static const char *const sub_topics[] = {
	"s/ds", "s/e", "s/dat", "devicecontrol/notifications", "error",
};

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[2048];
static uint8_t tx_buf[2048];
static uint8_t payload_buf[2048];
static uint16_t next_msg_id = 1;
static uint32_t telemetry_sent;

static bool connack_seen;
static int connack_result;
static bool restart_requested;
#if defined(CONFIG_SPIKE_REMOTE_ACCESS)
static char ra_msg[192];
static bool ra_requested;
#endif
static bool restart_pending_after_boot;

/* The latest JWT (s/dat), for HTTPS requests to Cumulocity. */
static char jwt[1024];

const char *spike_mqtt_jwt(void)
{
	return jwt;
}

#if defined(CONFIG_SPIKE_OTA)
/* c8y_Firmware (515): "<name>,<version>,<url>" while an update is pending. */
static char fw_request[320];
static bool fw_requested;
static char fw_marker[320];
#endif

#if defined(CONFIG_SPIKE_AUTH_BOOTSTRAP)
/* Bootstrap (task 5.7): first as the bootstrap user, then as the device user
 * from "70,<tenant>,<user>,<password>".
 */
static char dev_user[96];
static char dev_password[96];
static struct mqtt_utf8 rt_user;
static struct mqtt_utf8 rt_password;
static bool bootstrapping;
static bool got_credentials;

static int basic_cb(const char *key, size_t len, settings_read_cb read_cb,
		    void *cb_arg, void *param)
{
	char buf[200];

	ARG_UNUSED(key);
	ARG_UNUSED(param);
	if (len < sizeof(buf)) {
		char *sep;

		read_cb(cb_arg, buf, len);
		buf[len] = '\0';
		sep = strchr(buf, '\n');
		if (sep) {
			*sep = '\0';
			snprintk(dev_user, sizeof(dev_user), "%s", buf);
			snprintk(dev_password, sizeof(dev_password), "%s", sep + 1);
		}
	}
	return 0;
}
#endif

#if defined(CONFIG_SPIKE_AUTH_BASIC)
static struct mqtt_utf8 basic_user = {
	.utf8 = (const uint8_t *)CONFIG_SPIKE_BASIC_USER,
	.size = sizeof(CONFIG_SPIKE_BASIC_USER) - 1,
};
static struct mqtt_utf8 basic_password = {
	.utf8 = (const uint8_t *)CONFIG_SPIKE_BASIC_PASSWORD,
	.size = sizeof(CONFIG_SPIKE_BASIC_PASSWORD) - 1,
};
#endif

K_THREAD_STACK_DEFINE(spike_mqtt_stack, CONFIG_SPIKE_MQTT_STACK_SIZE);
static struct k_thread spike_mqtt_thread;

/* ------------------------------------------------------------------------ */
/* Measurement helpers                                                        */
/* ------------------------------------------------------------------------ */

static void log_tls_heap(const char *when)
{
	size_t cur, cur_blocks, max, max_blocks;

	mbedtls_memory_buffer_alloc_cur_get(&cur, &cur_blocks);
	mbedtls_memory_buffer_alloc_max_get(&max, &max_blocks);
	LOG_INF("MEAS tls_heap %s: cur=%zu B (%zu blocks) peak=%zu B "
		"(%zu blocks) of %d B",
		when, cur, cur_blocks, max, max_blocks, CONFIG_MBEDTLS_HEAP_SIZE);
}

static void log_stack(const char *when)
{
	size_t unused = 0;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("MEAS stack %s: used=%zu of %d B", when,
			(size_t)CONFIG_SPIKE_MQTT_STACK_SIZE - unused,
			CONFIG_SPIKE_MQTT_STACK_SIZE);
	}
}

/* ------------------------------------------------------------------------ */
/* Restart marker (survives the reboot, like the pico client's "restarting")  */
/* ------------------------------------------------------------------------ */

static int restart_marker_cb(const char *key, size_t len,
			     settings_read_cb read_cb, void *cb_arg, void *param)
{
	uint8_t v = 0;

	ARG_UNUSED(key);
	if (len == sizeof(v) && read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
		*(bool *)param = (v == 1);
	}
	return 0;
}

#if defined(CONFIG_SPIKE_OTA)
static int fw_marker_cb(const char *key, size_t len, settings_read_cb read_cb,
			void *cb_arg, void *param)
{
	ARG_UNUSED(key);
	ARG_UNUSED(param);
	if (len > 1 && len < sizeof(fw_marker)) {
		read_cb(cb_arg, fw_marker, len);
		fw_marker[len] = '\0';
	}
	return 0;
}
#endif

static void restart_marker_load(void)
{
	settings_subsys_init();
#if defined(CONFIG_SPIKE_OTA)
	settings_load_subtree_direct("spike/fw", fw_marker_cb, NULL);
	if (fw_marker[0]) {
		LOG_INF("firmware update pending: %s", fw_marker);
	}
#endif
	settings_load_subtree_direct("spike/restart", restart_marker_cb,
				     &restart_pending_after_boot);
	if (restart_pending_after_boot) {
		LOG_INF("restart marker found: will report c8y_Restart SUCCESSFUL");
	}
}

/* ------------------------------------------------------------------------ */
/* MQTT                                                                       */
/* ------------------------------------------------------------------------ */

static int publish(const char *topic, const char *payload, enum mqtt_qos qos)
{
	struct mqtt_publish_param p = {
		.message.topic.topic.utf8 = (const uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.topic.qos = qos,
		.message.payload.data = (uint8_t *)payload,
		.message.payload.len = strlen(payload),
		.message_id = next_msg_id++,
	};
	int ret = mqtt_publish(&client, &p);

	if (ret) {
		LOG_ERR("publish %s failed: %d", topic, ret);
	}
	return ret;
}

static void handle_message(const char *topic, const char *payload, size_t len)
{
	if (strcmp(topic, "s/dat") == 0) {
		/* "71,<jwt>": never log the token itself. */
		LOG_INF("s/dat: JWT received (%zu bytes)", len > 3 ? len - 3 : 0);
		if (len > 3 && len - 3 < sizeof(jwt)) {
			memcpy(jwt, payload + 3, len - 3);
			jwt[len - 3] = '\0';
		}
		return;
	}
	if (strcmp(topic, "s/dcr") == 0 && strncmp(payload, "70,", 3) == 0) {
		LOG_INF("s/dcr: 70,<tenant>,<user>,<password> received (%zu B)", len);
	} else if (strncmp(payload, "530,", 4) == 0 ||
		   strstr(payload, "c8y_RemoteAccessConnect")) {
		LOG_INF("%s: remote-access request (%zu B, key not logged)", topic,
			len);
	} else {
		LOG_INF("%s: %.*s", topic, (int)MIN(len, 200), payload);
	}

	if (strcmp(topic, "s/ds") == 0 && strncmp(payload, "510,", 4) == 0) {
		restart_requested = true;
	}
#if defined(CONFIG_SPIKE_REMOTE_ACCESS)
	if (strcmp(topic, "s/ds") == 0 && strncmp(payload, "530,", 4) == 0) {
		snprintk(ra_msg, sizeof(ra_msg), "%s", payload);
		ra_requested = true;
	}
#endif
#if defined(CONFIG_SPIKE_AUTH_BOOTSTRAP)
	/* 70,<tenant>,<user>,<password> */
	if (strcmp(topic, "s/dcr") == 0 && strncmp(payload, "70,", 3) == 0) {
		char buf[200];
		char *tenant = buf, *user, *password;

		snprintk(buf, sizeof(buf), "%s", payload + 3);
		user = strchr(tenant, ',');
		password = user ? strchr(user + 1, ',') : NULL;
		if (user && password) {
			*user++ = '\0';
			*password++ = '\0';
			snprintk(dev_user, sizeof(dev_user), "%s/%s", tenant, user);
			snprintk(dev_password, sizeof(dev_password), "%s", password);
			got_credentials = true;
		}
	}
#endif
#if defined(CONFIG_SPIKE_OTA)
	/* 515,<device>,<name>,<version>,<url> */
	if (strcmp(topic, "s/ds") == 0 && strncmp(payload, "515,", 4) == 0) {
		const char *rest = strchr(payload + 4, ',');

		if (rest) {
			snprintk(fw_request, sizeof(fw_request), "%s", rest + 1);
			fw_requested = true;
		}
	}
#endif
}

static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connack_seen = true;
		connack_result = evt->result;
		break;

	case MQTT_EVT_SUBACK: {
		const struct mqtt_suback_param *s = &evt->param.suback;

		for (size_t i = 0; i < s->return_codes.len; i++) {
			LOG_INF("SUBACK id=%u code=0x%02x", s->message_id,
				s->return_codes.data[i]);
		}
		break;
	}

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		size_t len = p->message.payload.len;
		char topic[64];
		size_t tlen = MIN(p->message.topic.topic.size, sizeof(topic) - 1);
		int ret;

		memcpy(topic, p->message.topic.topic.utf8, tlen);
		topic[tlen] = '\0';

		if (len >= sizeof(payload_buf)) {
			LOG_WRN("%s: %zu-byte payload too large, dropped", topic, len);
			while (len > 0) {
				ret = mqtt_read_publish_payload_blocking(
					c, payload_buf, MIN(len, sizeof(payload_buf)));
				if (ret <= 0) {
					break;
				}
				len -= ret;
			}
			break;
		}
		ret = mqtt_readall_publish_payload(c, payload_buf, len);
		if (ret) {
			LOG_ERR("reading %s payload failed: %d", topic, ret);
			break;
		}
		payload_buf[len] = '\0';
		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {.message_id = p->message_id};

			mqtt_publish_qos1_ack(c, &ack);
		}
		handle_message(topic, (const char *)payload_buf, len);
		break;
	}

	case MQTT_EVT_DISCONNECT:
		LOG_INF("disconnected (%d)", evt->result);
		break;

	default:
		break;
	}
}

static int resolve_broker(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port[8];
	int ret;

	snprintk(port, sizeof(port), "%d", CONFIG_SPIKE_C8Y_PORT);
	ret = zsock_getaddrinfo(CONFIG_SPIKE_C8Y_HOST, port, &hints, &res);
	if (ret) {
		LOG_ERR("DNS lookup of %s failed: %d", CONFIG_SPIKE_C8Y_HOST, ret);
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

static void client_setup(void)
{
	struct mqtt_sec_config *tls = &client.transport.tls.config;

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt;
	client.client_id.utf8 = (const uint8_t *)DEVICE_ID;
	client.client_id.size = strlen(DEVICE_ID);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.keepalive = CONFIG_MQTT_KEEPALIVE;
	client.clean_session = 1;
#if defined(CONFIG_SPIKE_AUTH_BASIC)
	client.user_name = &basic_user;
	client.password = &basic_password;
#elif defined(CONFIG_SPIKE_AUTH_BOOTSTRAP)
	{
		const char *u = bootstrapping ? CONFIG_SPIKE_BOOTSTRAP_USER : dev_user;
		const char *pw = bootstrapping ? CONFIG_SPIKE_BOOTSTRAP_PASSWORD
					       : dev_password;

		rt_user.utf8 = (const uint8_t *)u;
		rt_user.size = strlen(u);
		rt_password.utf8 = (const uint8_t *)pw;
		rt_password.size = strlen(pw);
		client.user_name = &rt_user;
		client.password = &rt_password;
	}
#endif

	client.transport.type = MQTT_TRANSPORT_SECURE;
	tls->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls->cipher_list = NULL;
	tls->sec_tag_list = sec_tags;
	tls->sec_tag_count = ARRAY_SIZE(sec_tags);
	tls->hostname = CONFIG_SPIKE_C8Y_HOST;
}

/* Process input for up to @p ms, or until @p done becomes true. */
static int pump(int ms, const bool *done)
{
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end && !(done && *done)) {
		struct zsock_pollfd fds = {
			.fd = client.transport.tls.sock,
			.events = ZSOCK_POLLIN,
		};
		int wait = (int)MIN(end - k_uptime_get(), 1000);
		int ret = zsock_poll(&fds, 1, MAX(wait, 0));

		if (ret < 0) {
			return -errno;
		}
		if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
			ret = mqtt_input(&client);
			if (ret) {
				return ret;
			}
		}
		if (ret > 0 && (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP))) {
			return -ECONNRESET;
		}
		ret = mqtt_live(&client);
		if (ret && ret != -EAGAIN) {
			return ret;
		}
	}
	return 0;
}

/* Connect and wait for CONNACK. Logs the timing and the handshake heap. */
static int connect_measured(const char *label)
{
	int64_t t0, t_dns, t_tls;
	int ret;

	t0 = k_uptime_get();
	ret = resolve_broker();
	if (ret) {
		return ret;
	}
	t_dns = k_uptime_get();

	client_setup();
	connack_seen = false;
	mbedtls_memory_buffer_alloc_max_reset();
	ret = mqtt_connect(&client);
	t_tls = k_uptime_get();
	if (ret) {
		LOG_ERR("%s: mqtt_connect failed: %d (after %lld ms)", label, ret,
			t_tls - t_dns);
		log_tls_heap("after failed connect");
		return ret;
	}

	ret = pump(10000, &connack_seen);
	if (ret || !connack_seen || connack_result != 0) {
		LOG_ERR("%s: no CONNACK (ret=%d seen=%d result=%d)", label, ret,
			connack_seen, connack_result);
		mqtt_abort(&client);
		return ret ? ret : -ECONNREFUSED;
	}

	LOG_INF("MEAS connect %s: dns=%lld ms tcp+tls=%lld ms connack=%lld ms "
		"total=%lld ms (%s:%d)",
		label, t_dns - t0, t_tls - t_dns, k_uptime_get() - t_tls,
		k_uptime_get() - t0, CONFIG_SPIKE_C8Y_HOST, CONFIG_SPIKE_C8Y_PORT);
	log_tls_heap("after handshake (peak = handshake)");
	return 0;
}

static void subscribe_one(const char *topic)
{
	struct mqtt_topic t = {
		.topic.utf8 = (const uint8_t *)topic,
		.topic.size = strlen(topic),
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	struct mqtt_subscription_list list = {
		.list = &t,
		.list_count = 1,
		.message_id = next_msg_id++,
	};
	int ret = mqtt_subscribe(&client, &list);

	if (ret) {
		LOG_ERR("subscribe %s failed: %d", topic, ret);
	} else {
		LOG_INF("SUBSCRIBE id=%u %s", list.message_id, topic);
	}
	/* Let the SUBACK arrive before the next one. */
	pump(300, NULL);
}

static void on_connected(void)
{
	char line[96];

	for (size_t i = 0; i < ARRAY_SIZE(sub_topics); i++) {
		subscribe_one(sub_topics[i]);
	}

	snprintk(line, sizeof(line), "100,%s,thin-edge.io-zephyr-spike",
		 DEVICE_ID);
	publish("s/us", line, MQTT_QOS_1_AT_LEAST_ONCE);
#if defined(CONFIG_SPIKE_OTA)
	publish("s/us", IS_ENABLED(CONFIG_SPIKE_REMOTE_ACCESS)
				? "114,c8y_Restart,c8y_Firmware,c8y_RemoteAccessConnect"
				: "114,c8y_Restart,c8y_Firmware",
		MQTT_QOS_1_AT_LEAST_ONCE);
	{
		char ver[24] = "unknown";

		spike_ota_running_version(ver, sizeof(ver));
		snprintk(line, sizeof(line), "115,%s,%s", CONFIG_APP_FIRMWARE_NAME,
			 ver);
		publish("s/us", line, MQTT_QOS_1_AT_LEAST_ONCE);
	}
#else
	publish("s/us", "114,c8y_Restart", MQTT_QOS_1_AT_LEAST_ONCE);
#endif
	publish("s/us", "117,60", MQTT_QOS_1_AT_LEAST_ONCE);

	if (restart_pending_after_boot) {
		uint8_t zero = 0;

		publish("s/us", "503,c8y_Restart", MQTT_QOS_1_AT_LEAST_ONCE);
		settings_save_one("spike/restart", &zero, sizeof(zero));
		restart_pending_after_boot = false;
		LOG_INF("reported c8y_Restart SUCCESSFUL after reboot");
	}

	publish("s/uat", "", MQTT_QOS_1_AT_LEAST_ONCE);
	pump(2000, NULL);

#if defined(CONFIG_SPIKE_OTA)
	/* A firmware update was pending across the reboot. */
	if (fw_marker[0]) {
		char out[400];
		bool done = true;

		if (!boot_is_img_confirmed()) {
			if (IS_ENABLED(CONFIG_SPIKE_OTA_AUTO_CONFIRM)) {
				/* Cloud reachable: this is the health gate. */
				int ret = boot_write_img_confirmed();

				LOG_INF("MEAS ota: image confirmed after reaching "
					"Cumulocity at uptime %lld ms (%d)",
					k_uptime_get(), ret);
				snprintk(out, sizeof(out), "115,%s", fw_marker);
				publish("s/us", out, MQTT_QOS_1_AT_LEAST_ONCE);
				publish("s/us", "503,c8y_Firmware",
					MQTT_QOS_1_AT_LEAST_ONCE);
			} else {
				LOG_INF("ota: test image running, NOT confirmed "
					"(auto-confirm off); a reset reverts it");
				done = false;
			}
		} else {
			/* Running a confirmed image although an update was
			 * pending: MCUboot reverted the new one.
			 */
			LOG_WRN("ota: update did not stick; MCUboot rolled back");
			publish("s/us",
				"502,c8y_Firmware,\"new image did not confirm; "
				"rolled back to the previous image\"",
				MQTT_QOS_1_AT_LEAST_ONCE);
		}
		if (done) {
			settings_delete("spike/fw");
			fw_marker[0] = '\0';
		}
		pump(1000, NULL);
	}
#endif
}

#if defined(CONFIG_SPIKE_OTA)
static void do_firmware(void)
{
	char req[sizeof(fw_request)];
	char *url;
	char reason[64];
	int ret;

	fw_requested = false;
	snprintk(req, sizeof(req), "%s", fw_request);
	/* "<name>,<version>,<url>" */
	url = strchr(req, ',');
	url = url ? strchr(url + 1, ',') : NULL;
	if (!url) {
		publish("s/us", "502,c8y_Firmware,\"bad 515 message\"",
			MQTT_QOS_1_AT_LEAST_ONCE);
		return;
	}
	url++;

	LOG_INF("c8y_Firmware: EXECUTING (%s)", req);
	publish("s/us", "501,c8y_Firmware", MQTT_QOS_1_AT_LEAST_ONCE);
	pump(500, NULL);

	/* The JWT only goes to Cumulocity itself. Binary URLs use the tenant-ID
	 * host (t<id>.<domain>), not the tenant's own host name, so compare the
	 * parent domain: "tedge-dev05.preprod.c8y.io" -> ".preprod.c8y.io".
	 */
	const char *parent = strchr(CONFIG_SPIKE_C8Y_HOST, '.');
	const char *host = strstr(url, "://");
	const char *host_end = host ? strpbrk(host + 3, ":/") : NULL;
	bool to_c8y = false;

	if (parent && host && host_end) {
		size_t plen = strlen(parent);
		size_t hlen = host_end - (host + 3);

		to_c8y = hlen > plen &&
			 strncmp(host_end - plen, parent, plen) == 0;
	}
	LOG_INF("firmware URL is %s Cumulocity: %s the JWT",
		to_c8y ? "on" : "not on", to_c8y ? "sending" : "not sending");

	ret = spike_ota_download(url, (to_c8y && jwt[0]) ? jwt : NULL);
	log_tls_heap("after firmware download (MQTT + HTTPS)");
	if (ret) {
		snprintk(reason, sizeof(reason),
			 "502,c8y_Firmware,\"download failed: %d\"", ret);
		publish("s/us", reason, MQTT_QOS_1_AT_LEAST_ONCE);
		pump(500, NULL);
		return;
	}
	settings_save_one("spike/fw", req, strlen(req));
	mqtt_disconnect(&client, NULL);
	k_sleep(K_MSEC(300));
	spike_ota_request_test_and_reboot();
}
#endif

static void do_restart(void)
{
	uint8_t one = 1;

	LOG_INF("c8y_Restart: EXECUTING, then reboot");
	publish("s/us", "501,c8y_Restart", MQTT_QOS_1_AT_LEAST_ONCE);
	pump(2000, NULL);
	settings_save_one("spike/restart", &one, sizeof(one));
	mqtt_disconnect(&client, NULL);
	k_sleep(K_MSEC(500));
	/* A full-system reset: after sys_reboot()'s CPU reset with Wi-Fi up,
	 * the C6 hangs in MCUboot until a power cycle (seen in this spike).
	 */
	boot_request_reboot();
}

/* Formats v with two decimals without floating-point printf support. */
static void fmt_fixed2(char *buf, size_t len, double v)
{
	long scaled = (long)(v * 100.0 + (v >= 0 ? 0.5 : -0.5));
	long whole = scaled / 100;
	long frac = scaled % 100;

	snprintk(buf, len, "%s%ld.%02ld", (scaled < 0 && whole == 0) ? "-" : "",
		 whole, frac < 0 ? -frac : frac);
}

static void publish_telemetry(void)
{
	size_t n = data_source_count();
	char value[16];

#if defined(CONFIG_SPIKE_ENDPOINT_MQTT_SERVICE)
	/* Free-form, in thin-edge.io's measurement shape. */
	char json[256];
	size_t off = 0;

	off += snprintk(json + off, sizeof(json) - off, "{");
	for (size_t i = 0; i < n && off < sizeof(json); i++) {
		const struct data_measurement *d = data_source_descriptor(i);

		fmt_fixed2(value, sizeof(value), data_source_sample(i));
		off += snprintk(json + off, sizeof(json) - off, "%s\"%s\":%s",
				i ? "," : "", d->name, value);
	}
	if (off < sizeof(json)) {
		snprintk(json + off, sizeof(json) - off, "}");
	}
	char topic[96];

	snprintk(topic, sizeof(topic), "te/device/%s///m/environment", DEVICE_ID);
	if (publish(topic, json,
		    MQTT_QOS_0_AT_MOST_ONCE) == 0 && telemetry_sent++ == 0) {
		LOG_INF("telemetry: first free-form publish: %s", json);
	}
#else
	/* Core MQTT: one SmartREST 200 per series. */
	char line[96];

	for (size_t i = 0; i < n; i++) {
		const struct data_measurement *d = data_source_descriptor(i);

		fmt_fixed2(value, sizeof(value), data_source_sample(i));
		snprintk(line, sizeof(line), "200,c8y_Spike,%s,%s,%s", d->name,
			 value, d->unit ? d->unit : "");
		publish("s/us", line, MQTT_QOS_0_AT_MOST_ONCE);
		if (telemetry_sent++ == 0) {
			LOG_INF("telemetry: first SmartREST publish: %s", line);
		}
	}
#endif
}

static void spike_mqtt_main(void *a, void *b, void *c)
{
	int64_t last_telemetry = 0, last_heap = 0, t_lost = 0;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	restart_marker_load();
	spike_time_sync();
#if defined(CONFIG_SPIKE_AUTH_ENROLLED)
	if (spike_enroll_run(device_id, sizeof(device_id))) {
		LOG_ERR("enrollment failed; not connecting");
		return;
	}
#endif
#if defined(CONFIG_SPIKE_AUTH_BOOTSTRAP)
	settings_load_subtree_direct("spike/basic", basic_cb, NULL);
	if (dev_user[0]) {
		LOG_INF("bootstrap: device credentials found in settings (%s)",
			dev_user);
	} else {
		int64_t t0 = k_uptime_get();
		int polls = 0;

		bootstrapping = true;
		while (connect_measured("bootstrap") != 0) {
			k_sleep(K_SECONDS(5));
		}
		subscribe_one("s/dcr");
		while (!got_credentials) {
			publish("s/ucr", "", MQTT_QOS_1_AT_LEAST_ONCE);
			polls++;
			if (pump(5000, NULL)) {
				LOG_WRN("bootstrap connection lost; reconnecting");
				mqtt_abort(&client);
				while (connect_measured("bootstrap") != 0) {
					k_sleep(K_SECONDS(5));
				}
				subscribe_one("s/dcr");
			}
		}
		{
			char buf[200];
			size_t n = snprintk(buf, sizeof(buf), "%s\n%s", dev_user,
					    dev_password);

			settings_save_one("spike/basic", buf, n);
		}
		LOG_INF("MEAS bootstrap: device credentials after %d s/ucr polls, "
			"%lld ms; user %s", polls, k_uptime_get() - t0, dev_user);
		mqtt_disconnect(&client, NULL);
		pump(500, NULL);
		bootstrapping = false;
	}
#endif
	log_tls_heap("before first connect");

	for (int i = 1; i <= CONFIG_SPIKE_CONNECT_CYCLES; i++) {
		char label[16];

		snprintk(label, sizeof(label), "cycle %d", i);
		if (connect_measured(label) == 0) {
			mqtt_disconnect(&client, NULL);
			pump(500, NULL);
		}
		log_tls_heap("after disconnect");
		/* Let the previous TCP connection finish closing. */
		k_sleep(K_SECONDS(3));
	}

	for (;;) {
		ret = connect_measured("main");
		if (ret) {
			k_sleep(K_SECONDS(5));
			continue;
		}
		if (t_lost) {
			LOG_INF("MEAS reconnected %lld ms after the connection was lost",
				k_uptime_get() - t_lost);
			t_lost = 0;
		}
		log_stack("after handshake");
		on_connected();

		for (;;) {
			ret = pump(1000, NULL);
			if (ret) {
				LOG_WRN("connection lost: %d", ret);
				t_lost = k_uptime_get();
				mqtt_abort(&client);
				log_tls_heap("after connection lost");
				break;
			}
			if (restart_requested) {
				do_restart();
			}
#if defined(CONFIG_SPIKE_OTA)
			if (fw_requested) {
				do_firmware();
			}
#endif
#if defined(CONFIG_SPIKE_REMOTE_ACCESS)
			if (ra_requested) {
				char reason[96];
				char line[160];

				ra_requested = false;
				publish("s/us", "501,c8y_RemoteAccessConnect",
					MQTT_QOS_1_AT_LEAST_ONCE);
				if (spike_ra_request(ra_msg, reason, sizeof(reason))) {
					snprintk(line, sizeof(line),
						 "502,c8y_RemoteAccessConnect,\"%s\"", reason);
					publish("s/us", line, MQTT_QOS_1_AT_LEAST_ONCE);
				}
			}
			{
				struct spike_ra_event ev;
				char line[200];

				while (spike_ra_poll_event(&ev) == 0) {
					if (ev.type == SPIKE_RA_UP) {
						publish("s/us", "503,c8y_RemoteAccessConnect",
							MQTT_QOS_1_AT_LEAST_ONCE);
						snprintk(line, sizeof(line),
							 "400,c8y_RemoteAccessOpened,\"%s\"",
							 ev.text);
					} else if (ev.type == SPIKE_RA_FAILED) {
						snprintk(line, sizeof(line),
							 "502,c8y_RemoteAccessConnect,\"%s\"",
							 ev.text);
					} else {
						snprintk(line, sizeof(line),
							 "400,c8y_RemoteAccessClosed,\"%s\"",
							 ev.text);
					}
					publish("s/us", line, MQTT_QOS_1_AT_LEAST_ONCE);
				}
			}
#endif
			if (k_uptime_get() - last_telemetry >=
			    CONFIG_SPIKE_TELEMETRY_INTERVAL_S * 1000) {
				last_telemetry = k_uptime_get();
				publish_telemetry();
			}
			if (k_uptime_get() - last_heap >= 60000) {
				last_heap = k_uptime_get();
				log_tls_heap("steady");
				log_stack("steady");
				LOG_INF("MEAS telemetry messages sent: %u", telemetry_sent);
			}
		}
		k_sleep(K_SECONDS(2));
	}
}

void spike_mqtt_start(void)
{
	int ret;

	ret = tls_credential_add(TAG_SERVER_CA, TLS_CREDENTIAL_CA_CERTIFICATE,
				 server_ca, sizeof(server_ca));
#if defined(CONFIG_SPIKE_AUTH_CERT)
	ret = ret ?: tls_credential_add(TAG_DEVICE,
					TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
					device_crt, sizeof(device_crt));
	ret = ret ?: tls_credential_add(TAG_DEVICE, TLS_CREDENTIAL_PRIVATE_KEY,
					device_key, sizeof(device_key));
#endif
	if (ret) {
		LOG_ERR("adding TLS credentials failed: %d", ret);
		return;
	}

	LOG_INF("Spike A: %s:%d as %s (%s)", CONFIG_SPIKE_C8Y_HOST,
		CONFIG_SPIKE_C8Y_PORT,
		IS_ENABLED(CONFIG_SPIKE_AUTH_ENROLLED) ? "<enrolled ID>" : DEVICE_ID,
		IS_ENABLED(CONFIG_SPIKE_AUTH_BASIC) ? "basic auth" : "mutual TLS");

	k_thread_create(&spike_mqtt_thread, spike_mqtt_stack,
			K_THREAD_STACK_SIZEOF(spike_mqtt_stack), spike_mqtt_main,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&spike_mqtt_thread, "spike_mqtt");
}
