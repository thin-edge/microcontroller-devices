/* SPDX-License-Identifier: Apache-2.0
 *
 * The Cumulocity transport: one MQTTS session (the MQTT Service on 9883 with
 * a certificate, or Core MQTT on 8883), the SmartREST dispatcher, the JWT,
 * the twin/health publishing and the restart operation.
 *
 * Only the client thread (tedge_core.c) touches this file's state: Zephyr's
 * MQTT client is not thread-safe.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/clock.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

#if defined(CONFIG_TEDGE_C8Y_MQTT_SERVICE)
#define C8Y_PORT 9883
#define FREE_FORM_TOPICS 1
#else
#define C8Y_PORT 8883
#define FREE_FORM_TOPICS 0
#endif

#define CONNACK_TIMEOUT_MS 15000
#define SUBACK_RETRY_MAX   5
#define JWT_REFRESH_MS     (50 * 60 * 1000)
/* Three broker closes this soon after CONNACK look like another client using
 * the same ID (c8y-direct-spikes, Spike B finding 6). */
#define TAKEOVER_WINDOW_MS 10000
#define TAKEOVER_STRIKES   3

/* The trust anchors listed in CONFIG_TEDGE_C8Y_CA_FILES. */
static const unsigned char server_ca[] = {
#include "tedge_ca_bundle.inc"
	0x00
};

static const sec_tag_t sec_tags_mtls[] = { TEDGE_TAG_SERVER_CA, TEDGE_TAG_DEVICE };
static const sec_tag_t sec_tags_basic[] = { TEDGE_TAG_SERVER_CA };

/* Downstream topics: thin-edge.io's Cumulocity bridge set, minus the
 * bootstrap topics (tedge_bootstrap.c owns those). */
static const char *const sub_topics[] = { "s/ds", "s/e", "s/dat" };

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[2048];
static uint8_t tx_buf[2048];
static uint8_t payload_buf[1024];
static uint16_t next_msg_id = 1;
static bool session_open;
static bool connack_seen;
static int connack_result;
static char jwt[1024];
static int64_t jwt_at;
static char device_id[64];

static struct mqtt_utf8 basic_user;
static struct mqtt_utf8 basic_password;

static uint16_t last_suback_id;
static uint8_t last_suback_code;

static int64_t connected_at;
static int takeover_strikes;

/* Operations the application registered, plus the built-in ones. */
struct op_entry {
	const char *name;
	tedge_operation_handler_t handler;
	void *user_data;
};
#define OP_SLOTS 4
static struct op_entry ops[OP_SLOTS];

struct tedge_operation {
	char name[40];
	char payload[192];
	bool done;
};
static struct tedge_operation current_op;

#if defined(CONFIG_TEDGE_RESTART)
static bool restart_requested;
static bool restart_pending_after_boot;
#endif

/* ------------------------------------------------------------------------ */
/* Publishing                                                                */
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
	int ret;

	if (!session_open) {
		return -ENOTCONN;
	}
	ret = mqtt_publish(&client, &p);
	if (ret) {
		LOG_ERR("publish to %s failed (%d)", topic, ret);
	}
	return ret;
}

/* One operation at a time.
 *
 * Cumulocity's 501, 502 and 503 act on the *oldest* operation in the
 * matching state, not on an operation named in the message, so a device with
 * two operations executing at once gets their results crossed: the log
 * upload's URL lands on the command, and an operation is left EXECUTING for
 * ever. The client therefore runs one operation at a time and keeps the rest
 * in a small queue.
 *
 * The gate is maintained here, where every status message passes, rather
 * than at each of the places that send one: a feature added later cannot
 * forget it.
 */
#define OP_QUEUE_DEPTH 2
/* A firmware request carries a URL, which is the longest line that arrives. */
#define OP_LINE_MAX    768

static bool op_executing;
static char op_queue[OP_QUEUE_DEPTH][OP_LINE_MAX];
static uint8_t op_queued;

int tedge_c8y_publish_sr(const char *line)
{
	int tmpl = tedge_sr_template(line);

	if (tmpl == 501) {
		op_executing = true;
	} else if (tmpl == 502 || tmpl == 503) {
		op_executing = false;
	}
	return publish("s/us", line, MQTT_QOS_1_AT_LEAST_ONCE);
}

const char *tedge_c8y_jwt(void)
{
	return jwt;
}

/* ------------------------------------------------------------------------ */
/* Operations                                                                */
/* ------------------------------------------------------------------------ */

int tedge_register_operation(const char *name, tedge_operation_handler_t handler,
			     void *user_data)
{
	if (name == NULL || handler == NULL) {
		return -EINVAL;
	}
	for (int i = 0; i < OP_SLOTS; i++) {
		if (ops[i].name == NULL) {
			ops[i].name = name;
			ops[i].handler = handler;
			ops[i].user_data = user_data;
			return 0;
		}
	}
	LOG_ERR("no free operation slot for \"%s\" (%d)", name, OP_SLOTS);
	return -ENOMEM;
}

const char *tedge_operation_payload(const struct tedge_operation *op)
{
	return (op != NULL) ? op->payload : NULL;
}

static int op_result(struct tedge_operation *op, const char *sr)
{
	if (op == NULL || op->done) {
		return -EINVAL;
	}
	op->done = true;
	return tedge_c8y_publish_sr(sr);
}

int tedge_operation_succeed(struct tedge_operation *op, const char *result)
{
	char line[224];

	if (op == NULL) {
		return -EINVAL;
	}
	if (result != NULL && result[0] != '\0') {
		char quoted[160];

		(void)tedge_sr_quote(result, quoted, sizeof(quoted));
		snprintf(line, sizeof(line), "503,%s,%s", op->name, quoted);
	} else {
		snprintf(line, sizeof(line), "503,%s", op->name);
	}
	return op_result(op, line);
}

int tedge_operation_fail(struct tedge_operation *op, const char *reason)
{
	char line[224];
	char quoted[160];

	if (op == NULL) {
		return -EINVAL;
	}
	(void)tedge_sr_quote(reason ? reason : "failed", quoted, sizeof(quoted));
	snprintf(line, sizeof(line), "502,%s,%s", op->name, quoted);
	return op_result(op, line);
}

/* Report an operation the image cannot run (baseline requirement: never leave
 * it pending).
 *
 * 501 first, always: Cumulocity's 502 fails the oldest *executing* operation,
 * so refusing one straight from PENDING would leave it pending for ever —
 * exactly the state this function exists to avoid. */
static void op_unsupported(const char *name, const char *why)
{
	char line[224];
	char quoted[160];
	char text[140];

	snprintf(line, sizeof(line), "501,%s", name);
	(void)tedge_c8y_publish_sr(line);
	snprintf(text, sizeof(text), "%s", why);
	(void)tedge_sr_quote(text, quoted, sizeof(quoted));
	snprintf(line, sizeof(line), "502,%s,%s", name, quoted);
	(void)tedge_c8y_publish_sr(line);
}

/* ------------------------------------------------------------------------ */
/* Restart (CONFIG_TEDGE_RESTART)                                            */
/* ------------------------------------------------------------------------ */

#if defined(CONFIG_TEDGE_RESTART)
static void handle_restart(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();
	char reason[96] = "";
	uint8_t one = 1;

	restart_requested = false;
	if (hooks != NULL && hooks->restart_request != NULL) {
		int rc = hooks->restart_request(reason, sizeof(reason),
						hooks->user_data);

		if (rc != 0) {
			LOG_WRN("restart vetoed by the application (%d): %s", rc,
				reason[0] ? reason : "no reason given");
			op_unsupported("c8y_Restart",
				       reason[0] ? reason
						 : "the application refused the restart");
			return;
		}
	}
	LOG_INF("restart requested by the cloud");
	(void)tedge_c8y_publish_sr("501,c8y_Restart");
	(void)settings_save_one(TEDGE_KEY_RESTART, &one, sizeof(one));
	k_msleep(500);
	mqtt_disconnect(&client, NULL);
	k_msleep(300);
	tedge_platform_reset();
}

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
#endif /* CONFIG_TEDGE_RESTART */

/* ------------------------------------------------------------------------ */
/* Downstream messages                                                       */
/* ------------------------------------------------------------------------ */

static void dispatch_operation(const char *line);

/* Runs @p line now, or holds it until the operation in flight finishes.
 * Called on the client thread only. */
static void handle_operation(const char *line)
{
	int tmpl = tedge_sr_template(line);

	if (op_executing && tmpl >= 510 && tmpl <= 579) {
		if (op_queued >= OP_QUEUE_DEPTH) {
			/* Left PENDING deliberately: failing it would fail the
			 * operation that is actually running, because that is
			 * the one Cumulocity's 502 acts on. Cumulocity sends
			 * pending operations again when the device
			 * reconnects. */
			LOG_WRN("operation %d arrived while %d were already "
				"waiting; it stays pending until the next "
				"connect", tmpl, op_queued);
			return;
		}
		snprintf(op_queue[op_queued], OP_LINE_MAX, "%s", line);
		op_queued++;
		LOG_INF("operation %d waits: another one is running", tmpl);
		return;
	}
	dispatch_operation(line);
}

/* Starts the next waiting operation, if the last one has finished. */
static void run_queued_operation(void)
{
	char line[OP_LINE_MAX];

	if (op_executing || op_queued == 0) {
		return;
	}
	memcpy(line, op_queue[0], sizeof(line));
	op_queued--;
	if (op_queued > 0) {
		memmove(op_queue[0], op_queue[1], OP_LINE_MAX);
	}
	dispatch_operation(line);
}

static void dispatch_operation(const char *line)
{
	char op_name[40];
	int tmpl = tedge_sr_template(line);

	switch (tmpl) {
	case 510: /* c8y_Restart */
#if defined(CONFIG_TEDGE_RESTART)
		restart_requested = true;
#else
		op_unsupported("c8y_Restart",
			       "restart is not built into this image "
			       "(CONFIG_TEDGE_RESTART)");
#endif
		return;
	case 515:
#if defined(CONFIG_TEDGE_FIRMWARE_UPDATE)
	{
		char reason[128];
		char sr[224];
		char quoted[160];

		/* 501 first, always: Cumulocity's 502 fails the oldest
		 * *executing* operation, so an operation refused straight from
		 * PENDING would never leave that state. */
		(void)tedge_c8y_publish_sr("501,c8y_Firmware");
		if (tedge_fw_request(line, reason, sizeof(reason)) != 0) {
			(void)tedge_sr_quote(reason, quoted, sizeof(quoted));
			snprintf(sr, sizeof(sr), "502,c8y_Firmware,%s", quoted);
			(void)tedge_c8y_publish_sr(sr);
		} else {
			(void)tedge_sr_quote(tedge_fw_downtime_hint(), quoted,
					     sizeof(quoted));
			snprintf(sr, sizeof(sr), "400,c8y_FirmwareUpdateStarted,%s",
				 quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#else
		op_unsupported("c8y_Firmware",
			       "firmware update is not built into this image "
			       "(CONFIG_TEDGE_FIRMWARE_UPDATE)");
#endif
		return;
	case 530:
#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
	{
		char reason[112];

		(void)tedge_c8y_publish_sr("501,c8y_RemoteAccessConnect");
		if (tedge_ra_request(line, reason, sizeof(reason)) != 0) {
			char quoted[128];

			(void)tedge_sr_quote(reason, quoted, sizeof(quoted));
			char sr[192];

			snprintf(sr, sizeof(sr), "502,c8y_RemoteAccessConnect,%s",
				 quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#else
		op_unsupported("c8y_RemoteAccessConnect",
			       "remote access is not built into this image "
			       "(CONFIG_TEDGE_REMOTE_ACCESS)");
#endif
		return;
	case 511:
#if defined(CONFIG_TEDGE_SHELL_COMMAND)
	{
		char reason[160];
		char sr[288];
		char quoted[200];

		/* 501 first, for the reason firmware update gives above. */
		(void)tedge_c8y_publish_sr("501,c8y_Command");
		if (tedge_shell_request(line, reason, sizeof(reason)) != 0) {
			(void)tedge_sr_quote(reason, quoted, sizeof(quoted));
			snprintf(sr, sizeof(sr), "502,c8y_Command,%s", quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
		return;
	}
#else
		/* Without the feature the application may still register its
		 * own c8y_Command handler, so fall through to the registered
		 * operations rather than refusing here. */
		snprintf(op_name, sizeof(op_name), "c8y_Command");
		break;
#endif
	case 522:
#if defined(CONFIG_TEDGE_LOG_UPLOAD)
	{
		char reason[128];
		char sr[224];
		char quoted[160];

		/* 501 first, for the reason firmware update gives above. */
		(void)tedge_c8y_publish_sr("501,c8y_LogfileRequest");
		if (tedge_log_request(line, reason, sizeof(reason)) != 0) {
			(void)tedge_sr_quote(reason, quoted, sizeof(quoted));
			snprintf(sr, sizeof(sr), "502,c8y_LogfileRequest,%s",
				 quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#else
		op_unsupported("c8y_LogfileRequest",
			       "log upload is not built into this image "
			       "(CONFIG_TEDGE_LOG_UPLOAD)");
#endif
		return;
	default:
		if (tmpl < 0) {
			return; /* not a SmartREST line */
		}
		snprintf(op_name, sizeof(op_name), "%d", tmpl);
		break;
	}

	for (int i = 0; i < OP_SLOTS; i++) {
		if (ops[i].name != NULL && strcmp(ops[i].name, op_name) == 0) {
			char line_sr[64];

			memset(&current_op, 0, sizeof(current_op));
			snprintf(current_op.name, sizeof(current_op.name), "%s",
				 op_name);
			snprintf(current_op.payload, sizeof(current_op.payload),
				 "%.*s", (int)sizeof(current_op.payload) - 1,
				 line);
			snprintf(line_sr, sizeof(line_sr), "501,%s", op_name);
			(void)tedge_c8y_publish_sr(line_sr);
			ops[i].handler(&current_op, ops[i].user_data);
			return;
		}
	}
	/* Cumulocity's operations live in the 5xx templates. One this image
	 * has no handler for is failed with a reason, never dropped: an
	 * operation nobody answers stays pending in the cloud for ever, and
	 * the device looks broken rather than merely incomplete. Anything
	 * else on this topic is a response, not an operation. */
	if (tmpl >= 510 && tmpl <= 579) {
		char why[96];

		snprintf(why, sizeof(why),
			 "this image has no handler for SmartREST %d", tmpl);
		op_unsupported(op_name, why);
		return;
	}
	LOG_DBG("unhandled downstream message: %.40s", line);
}

static void handle_message(const char *topic, const char *payload, size_t len)
{
	if (strcmp(topic, "s/dat") == 0) {
		/* "71,<jwt>"; the token itself is never logged. */
		if (len > 3 && len - 3 < sizeof(jwt)) {
			memcpy(jwt, payload + 3, len - 3);
			jwt[len - 3] = '\0';
			jwt_at = k_uptime_get();
			LOG_DBG("token received (%zu B)", len - 3);
		}
		return;
	}
	if (strcmp(topic, "s/e") == 0) {
		LOG_WRN("cloud error: %.*s", (int)MIN(len, 160), payload);
		return;
	}
	if (strcmp(topic, "s/ds") == 0) {
		LOG_DBG("s/ds: %.60s", payload);
		handle_operation(payload);
	}
}

static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connack_seen = true;
		connack_result = evt->result;
		break;

	case MQTT_EVT_SUBACK:
		last_suback_id = evt->param.suback.message_id;
		last_suback_code = evt->param.suback.return_codes.len
					   ? evt->param.suback.return_codes.data[0]
					   : 0x80;
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		size_t len = p->message.payload.len;
		char topic[48];
		size_t tlen = MIN(p->message.topic.topic.size, sizeof(topic) - 1);
		int ret;

		memcpy(topic, p->message.topic.topic.utf8, tlen);
		topic[tlen] = '\0';

		if (len >= sizeof(payload_buf)) {
			LOG_WRN("%s: %zu-byte message dropped (buffer is %zu)",
				topic, len, sizeof(payload_buf));
			while (len > 0) {
				ret = mqtt_read_publish_payload_blocking(
					c, payload_buf,
					MIN(len, sizeof(payload_buf)));
				if (ret <= 0) {
					break;
				}
				len -= ret;
			}
			break;
		}
		ret = mqtt_readall_publish_payload(c, payload_buf, len);
		if (ret) {
			LOG_ERR("reading the %s payload failed (%d)", topic, ret);
			break;
		}
		payload_buf[len] = '\0';
		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = p->message_id,
			};

			mqtt_publish_qos1_ack(c, &ack);
		}
		handle_message(topic, (const char *)payload_buf, len);
		break;
	}

	case MQTT_EVT_DISCONNECT:
		session_open = false;
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

/* Read input (and keep the connection alive) for up to @p ms, or until
 * @p done. */
static int pump(int ms, const bool *done)
{
	int64_t end = k_uptime_get() + ms;

	do {
		struct zsock_pollfd fds = {
			.fd = client.transport.tls.sock,
			.events = ZSOCK_POLLIN,
		};
		int wait = (int)CLAMP(end - k_uptime_get(), 0, 1000);
		int ret = zsock_poll(&fds, 1, wait);

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
	} while (k_uptime_get() < end && !(done && *done));
	return 0;
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

	snprintf(port, sizeof(port), "%d", C8Y_PORT);
	ret = zsock_getaddrinfo(tedge_c8y_host(), port, &hints, &res);
	if (ret != 0) {
		LOG_ERR("DNS lookup of %s failed (%d)", tedge_c8y_host(), ret);
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

static void client_setup(void)
{
	struct mqtt_sec_config *tls = &client.transport.tls.config;
	const char *user = tedge_auth_username();

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt;
	client.client_id.utf8 = (const uint8_t *)device_id;
	client.client_id.size = strlen(device_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.keepalive = 60;
	client.clean_session = 1;

	if (user != NULL) {
		basic_user.utf8 = (const uint8_t *)user;
		basic_user.size = strlen(user);
		basic_password.utf8 = (const uint8_t *)tedge_auth_password();
		basic_password.size = strlen(tedge_auth_password());
		client.user_name = &basic_user;
		client.password = &basic_password;
	}

	client.transport.type = MQTT_TRANSPORT_SECURE;
	tls->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls->cipher_list = NULL;
	if (user != NULL) {
		tls->sec_tag_list = sec_tags_basic;
		tls->sec_tag_count = ARRAY_SIZE(sec_tags_basic);
	} else {
		tls->sec_tag_list = sec_tags_mtls;
		tls->sec_tag_count = ARRAY_SIZE(sec_tags_mtls);
	}
	tls->hostname = tedge_c8y_host();
}

/* Subscribe to one filter (the MQTT Service refuses several per packet).
 * Cumulocity refuses a subscription with 0x80 while the device does not exist
 * yet, so retry rather than give up (spikes P8).
 */
static int subscribe_one(const char *topic)
{
	for (int attempt = 1; attempt <= SUBACK_RETRY_MAX; attempt++) {
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
		int ret;

		last_suback_id = 0;
		last_suback_code = 0;
		ret = mqtt_subscribe(&client, &list);
		if (ret != 0) {
			LOG_ERR("subscribing to %s failed (%d)", topic, ret);
			return ret;
		}
		for (int i = 0; i < 10 && last_suback_id != list.message_id; i++) {
			ret = pump(300, NULL);
			if (ret != 0) {
				return ret;
			}
		}
		if (last_suback_id == list.message_id && last_suback_code != 0x80) {
			return 0;
		}
		LOG_WRN("subscription to %s refused (attempt %d of %d); the "
			"device may not exist yet", topic, attempt,
			SUBACK_RETRY_MAX);
		ret = pump(5000, NULL); /* let 100 be processed, then retry */
		if (ret != 0) {
			return ret;
		}
	}
	LOG_ERR("could not subscribe to %s", topic);
	return -EACCES;
}

/* The operations this image can actually run. */
static void publish_supported_ops(void)
{
	char line[160] = "114";
	size_t n = strlen(line);

	if (IS_ENABLED(CONFIG_TEDGE_RESTART)) {
		n += snprintf(line + n, sizeof(line) - n, ",c8y_Restart");
	}
	if (IS_ENABLED(CONFIG_TEDGE_REMOTE_ACCESS)) {
		n += snprintf(line + n, sizeof(line) - n,
			      ",c8y_RemoteAccessConnect");
	}
	if (IS_ENABLED(CONFIG_TEDGE_FIRMWARE_UPDATE)) {
		n += snprintf(line + n, sizeof(line) - n, ",c8y_Firmware");
	}
	if (IS_ENABLED(CONFIG_TEDGE_SHELL_COMMAND)) {
		n += snprintf(line + n, sizeof(line) - n, ",c8y_Command");
	}
	if (IS_ENABLED(CONFIG_TEDGE_LOG_UPLOAD)) {
		n += snprintf(line + n, sizeof(line) - n,
			      ",c8y_LogfileRequest");
	}
	for (int i = 0; i < OP_SLOTS && n < sizeof(line); i++) {
		if (ops[i].name != NULL) {
			n += snprintf(line + n, sizeof(line) - n, ",%s",
				      ops[i].name);
		}
	}
	if (n > 3) {
		(void)tedge_c8y_publish_sr(line);
	}

#if defined(CONFIG_TEDGE_LOG_UPLOAD)
	/* The log types this image can actually produce, so an operator is
	 * never offered one the device does not have. */
	{
		char logs[160];

		if (tedge_log_types_line(logs, sizeof(logs)) > 0) {
			(void)tedge_c8y_publish_sr(logs);
		}
	}
#endif
}

static int publish_twin_impl(const char *fragment, const char *json)
{
	char topic[128];
	int rc;

	if (FREE_FORM_TOPICS) {
		snprintf(topic, sizeof(topic), "te/device/%s///twin/%s", device_id,
			 fragment);
		rc = publish(topic, json, MQTT_QOS_1_AT_LEAST_ONCE);
	} else {
		/* Core MQTT has no free-form topics: a direct inventory update,
		 * as thin-edge.io's mapper does. */
		char body[320];

		snprintf(topic, sizeof(topic),
			 "inventory/managedObjects/update/%s", device_id);
		snprintf(body, sizeof(body), "{\"%s\":%s}", fragment, json);
		rc = publish(topic, body, MQTT_QOS_1_AT_LEAST_ONCE);
	}
	if (rc == 0) {
		LOG_INF("twin %s: %s", fragment, json);
	}
	return rc;
}

/* Progress: free-form topic, at most once. Nothing on Core MQTT, which has
 * no free-form topics; the operation status is the report there. */
int tedge_c8y_publish_progress(const char *kind, const char *json)
{
	char topic[128];

	if (!FREE_FORM_TOPICS) {
		return -ENOTSUP;
	}
	snprintf(topic, sizeof(topic), "te/device/%s///progress/%s", device_id,
		 kind);
	LOG_DBG("progress/%s: %s", kind, json);
	return publish(topic, json, MQTT_QOS_0_AT_MOST_ONCE);
}

#if defined(CONFIG_TEDGE_TELEMETRY)
/* Core MQTT has no free-form topics: one SmartREST 200 per series, taken
 * back out of the JSON the API built. */
static int smartrest_measurement(const char *type, const char *payload)
{
	size_t pos = 0;
	char series[40], value[24], line[128], when[32];
	int sent = 0;

	/* Static template 200 takes ",<unit>,<time>" after the value. The
	 * unit stays empty (the API's unit is advisory and the JSON does not
	 * carry it); the time is what makes a buffered reading truthful. */
	tedge_json_field(payload, "time", when, sizeof(when));
	while (tedge_json_next_number(payload, &pos, series, sizeof(series),
				      value, sizeof(value)) == 1) {
		snprintf(line, sizeof(line), "200,%s,%s,%s,,%s", type, series,
			 value, when);
		if (tedge_c8y_publish_sr(line) != 0) {
			return -ENOTCONN;
		}
		sent++;
	}
	return (sent > 0) ? 0 : -EINVAL;
}

/* Free-form te/ topics where the transport has them, SmartREST where it
 * does not; the application sees no difference. */
int tedge_c8y_publish_telemetry(enum tedge_msg_kind kind, const char *type,
				const char *payload)
{
	char topic[128];
	char line[256];

	if (!session_open) {
		return -ENOTCONN;
	}
	if (FREE_FORM_TOPICS) {
		static const char *const kinds[] = { "m", "e", "a", "a" };

		snprintf(topic, sizeof(topic), "te/device/%s///%s/%s", device_id,
			 kinds[kind], type);
		LOG_DBG("%s: %s", topic + strlen(topic) - strlen(type) - 3,
			payload);
		/* A measurement lost in a reconnect is one reading; an event or
		 * an alarm carries meaning, so those go at least once. */
		return publish(topic, payload,
			       (kind == TEDGE_MSG_MEASUREMENT)
				       ? MQTT_QOS_0_AT_MOST_ONCE
				       : MQTT_QOS_1_AT_LEAST_ONCE);
	}

	/* Core MQTT: SmartREST. The payload is JSON either way, so the
	 * fields are taken back out of it here. */
	switch (kind) {
	case TEDGE_MSG_MEASUREMENT:
		return smartrest_measurement(type, payload);
	case TEDGE_MSG_EVENT: {
		char text[160], quoted[180], when[32];

		tedge_json_field(payload, "text", text, sizeof(text));
		tedge_json_field(payload, "time", when, sizeof(when));
		(void)tedge_sr_quote(text, quoted, sizeof(quoted));
		snprintf(line, sizeof(line), "400,%s,%s,%s", type, quoted,
			 when);
		return tedge_c8y_publish_sr(line);
	}
	case TEDGE_MSG_ALARM: {
		char text[160], quoted[180], severity[16], when[32];
		static const struct {
			const char *name;
			const char *template;
		} map[] = {
			{ "critical", "301" }, { "major", "302" },
			{ "minor", "303" },    { "warning", "304" },
		};
		const char *tmpl = "304";

		tedge_json_field(payload, "severity", severity, sizeof(severity));
		tedge_json_field(payload, "text", text, sizeof(text));
		tedge_json_field(payload, "time", when, sizeof(when));
		for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
			if (strcmp(severity, map[i].name) == 0) {
				tmpl = map[i].template;
				break;
			}
		}
		(void)tedge_sr_quote(text, quoted, sizeof(quoted));
		snprintf(line, sizeof(line), "%s,%s,%s,%s", tmpl, type, quoted,
			 when);
		return tedge_c8y_publish_sr(line);
	}
	case TEDGE_MSG_ALARM_CLEAR:
	default:
		snprintf(line, sizeof(line), "306,%s", type);
		return tedge_c8y_publish_sr(line);
	}
}
#endif /* CONFIG_TEDGE_TELEMETRY */

static void publish_health(void)
{
	char topic[128];
	char body[96];
	struct timespec ts;

	if (!FREE_FORM_TOPICS) {
		return;
	}
	(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &ts);
	snprintf(topic, sizeof(topic), "te/device/%s/service/tedge-zephyr/status/health",
		 device_id);
	snprintf(body, sizeof(body), "{\"status\":\"up\",\"time\":%lld}",
		 (long long)ts.tv_sec);
	(void)publish(topic, body, MQTT_QOS_1_AT_LEAST_ONCE);
}

/* 100, subscriptions, 114, 117, s/uat, then state (design D6). */
static int session_start(void)
{
	/* A new session: Cumulocity sends its pending operations again, so
	 * whatever this client thought it was running is no longer true. */
	op_executing = false;
	op_queued = 0;

	const struct tedge_id *id = tedge_identity();
	char line[160];
	char agent[160];

	snprintf(line, sizeof(line), "100,%s,%s", id->name, id->type);
	(void)tedge_c8y_publish_sr(line);
	(void)pump(500, NULL);

	for (size_t i = 0; i < ARRAY_SIZE(sub_topics); i++) {
		int rc = subscribe_one(sub_topics[i]);

		if (rc != 0) {
			return rc;
		}
	}

	publish_supported_ops();
	if (CONFIG_TEDGE_REQUIRED_INTERVAL_MIN > 0) {
		snprintf(line, sizeof(line), "117,%d",
			 CONFIG_TEDGE_REQUIRED_INTERVAL_MIN);
		(void)tedge_c8y_publish_sr(line);
	}
	if (tedge_auth_username() == NULL) {
		/* Only certificate devices get a token. */
		(void)publish("s/uat", "", MQTT_QOS_0_AT_MOST_ONCE);
	}

#if defined(CONFIG_TEDGE_FIRMWARE_UPDATE)
	{
		char ver[24] = "";
		char fw[96];

		if (tedge_fw_running_version(ver, sizeof(ver)) == 0) {
			LOG_INF("firmware: running %s %s", id->firmware_name, ver);
			snprintf(fw, sizeof(fw), "115,%s,%s", id->firmware_name,
				 ver);
			(void)tedge_c8y_publish_sr(fw);
		} else {
			LOG_WRN("firmware: cannot read the running version");
		}
	}
#endif
	snprintf(agent, sizeof(agent),
		 "{\"name\":\"tedge-zephyr\",\"version\":\"%s\","
		 "\"transport\":\"%s\",\"firmware\":\"%s %s\"}",
		 tedge_version(), FREE_FORM_TOPICS ? "c8y-mqtt-service" : "c8y-core-mqtt",
		 id->firmware_name, id->firmware_version);
	(void)publish_twin_impl("tedge_Agent", agent);
#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
	{
		char ra[224];

		/* State, not an event: a reboot must not leave a stale count. */
		if (tedge_ra_twin(ra, sizeof(ra)) == 0) {
			(void)tedge_publish_twin("tedge_RemoteAccess", ra);
		}
	}
#endif
	publish_health();

#if defined(CONFIG_TEDGE_FIRMWARE_UPDATE)
	/* Confirm a test boot, or report an image MCUboot rolled back. */
	tedge_fw_on_connected();
#endif
#if defined(CONFIG_TEDGE_CERT_RENEWAL)
	{
		char cert[160];

		if (tedge_cert_twin(cert, sizeof(cert)) == 0) {
			(void)tedge_publish_twin("tedge_Certificate", cert);
		}
	}
#endif
#if defined(CONFIG_TEDGE_RESTART)
	if (restart_pending_after_boot) {
		(void)tedge_c8y_publish_sr("503,c8y_Restart");
		(void)settings_delete(TEDGE_KEY_RESTART);
		restart_pending_after_boot = false;
		LOG_INF("restart operation reported as successful");
	}
#endif
	return pump(500, NULL);
}

/* ------------------------------------------------------------------------ */
/* Transport interface                                                       */
/* ------------------------------------------------------------------------ */

static int c8y_connect(void)
{
	int64_t t0 = k_uptime_get();
	int ret;

	if (tedge_c8y_host()[0] == '\0') {
		LOG_ERR("no Cumulocity host: set CONFIG_TEDGE_C8Y_URL or call "
			"tedge_set_c8y_url()");
		return -EDESTADDRREQ;
	}

	ret = tls_credential_add(TEDGE_TAG_SERVER_CA, TLS_CREDENTIAL_CA_CERTIFICATE,
				 server_ca, sizeof(server_ca));
	if (ret != 0 && ret != -EEXIST) {
		LOG_ERR("could not register the trust anchor (%d)", ret);
		return ret;
	}

	ret = tedge_auth_prepare(device_id, sizeof(device_id));
	if (ret != 0) {
		return ret;
	}

	ret = resolve_broker();
	if (ret != 0) {
		return ret;
	}

	client_setup();
	connack_seen = false;
	connack_result = 0;
	ret = mqtt_connect(&client);
	if (ret != 0) {
		LOG_ERR("TLS/MQTT connect to %s:%d failed (%d)", tedge_c8y_host(),
			C8Y_PORT, ret);
		return ret;
	}
	session_open = true;

	ret = pump(CONNACK_TIMEOUT_MS, &connack_seen);
	if (ret != 0 || !connack_seen || connack_result != 0) {
		LOG_ERR("no CONNACK from %s:%d (ret=%d result=%d)",
			tedge_c8y_host(), C8Y_PORT, ret, connack_result);
		mqtt_abort(&client);
		session_open = false;
		return (ret != 0) ? ret : -ECONNREFUSED;
	}
	connected_at = k_uptime_get();
	LOG_INF("connected to %s:%d as \"%s\" in %lld ms", tedge_c8y_host(),
		C8Y_PORT, device_id, connected_at - t0);

	ret = session_start();
	if (ret != 0) {
		mqtt_abort(&client);
		session_open = false;
	}
	return ret;
}

static int c8y_poll(int timeout_ms)
{
	int ret = pump(timeout_ms, NULL);

	if (ret != 0) {
		return ret;
	}
	if (!session_open) {
		return -ENOTCONN;
	}
#if defined(CONFIG_TEDGE_RESTART)
	if (restart_requested) {
		handle_restart();
	}
#endif
#if defined(CONFIG_TEDGE_FIRMWARE_UPDATE)
	{
		struct tedge_fw_event ev;
		char sr[224], quoted[160];

		while (tedge_fw_poll_event(&ev) == 0) {
			(void)tedge_sr_quote(ev.text, quoted, sizeof(quoted));
			switch (ev.type) {
			case TEDGE_FW_INSTALLED: {
				char fw[96];

				snprintf(fw, sizeof(fw), "115,%s,%s",
					 tedge_identity()->firmware_name, ev.text);
				(void)tedge_c8y_publish_sr(fw);
				(void)tedge_c8y_publish_sr("503,c8y_Firmware");
				continue;
			}
			case TEDGE_FW_REBOOTING:
				snprintf(sr, sizeof(sr),
					 "400,c8y_FirmwareInstalling,%s", quoted);
				break;
			case TEDGE_FW_REVERTED:
			case TEDGE_FW_FAILED:
			default:
				snprintf(sr, sizeof(sr), "502,c8y_Firmware,%s",
					 quoted);
				break;
			}
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#endif
#if defined(CONFIG_TEDGE_SHELL_COMMAND)
	{
		struct tedge_shell_event ev;
		char sr[288], quoted[200];

		while (tedge_shell_poll_event(&ev) == 0) {
			/* SmartREST is one line: newlines become spaces and a
			 * long answer is cut. A command with a lot to say
			 * belongs behind a log type. */
			(void)tedge_sr_quote(ev.output, quoted, sizeof(quoted));
			snprintf(sr, sizeof(sr), "%s,c8y_Command,%s",
				 (ev.rc == 0) ? "503" : "502", quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#endif
#if defined(CONFIG_TEDGE_LOG_UPLOAD)
	{
		struct tedge_log_event ev;
		char sr[288], quoted[200];

		while (tedge_log_poll_event(&ev) == 0) {
			if (ev.rc == 0) {
				(void)tedge_sr_quote(ev.url, quoted,
						     sizeof(quoted));
				snprintf(sr, sizeof(sr),
					 "503,c8y_LogfileRequest,%s", quoted);
			} else {
				(void)tedge_sr_quote(ev.reason, quoted,
						     sizeof(quoted));
				snprintf(sr, sizeof(sr),
					 "502,c8y_LogfileRequest,%s", quoted);
			}
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#endif
#if defined(CONFIG_TEDGE_REMOTE_ACCESS)
	{
		struct tedge_ra_event ev;
		static int64_t twin_due;
		char sr[224], quoted[160];

		while (tedge_ra_poll_event(&ev) == 0) {
			(void)tedge_sr_quote(ev.text, quoted, sizeof(quoted));
			switch (ev.type) {
			case TEDGE_RA_UP:
				(void)tedge_c8y_publish_sr(
					"503,c8y_RemoteAccessConnect");
				snprintf(sr, sizeof(sr),
					 "400,c8y_RemoteAccessOpened,%s", quoted);
				break;
			case TEDGE_RA_FAILED:
				snprintf(sr, sizeof(sr),
					 "502,c8y_RemoteAccessConnect,%s", quoted);
				break;
			default:
				snprintf(sr, sizeof(sr),
					 "400,c8y_RemoteAccessClosed,%s", quoted);
				break;
			}
			(void)tedge_c8y_publish_sr(sr);
			/* The seat is freed just after the event is posted. */
			twin_due = k_uptime_get() + 1000;
		}
		if (twin_due != 0 && k_uptime_get() >= twin_due) {
			char ra[224];

			twin_due = 0;
			if (tedge_ra_twin(ra, sizeof(ra)) == 0) {
				(void)tedge_publish_twin("tedge_RemoteAccess", ra);
			}
		}
	}
#endif
	/* Anything that finished above freed the gate, so the next operation
	 * can start — on this thread, like every other dispatch. */
	run_queued_operation();
#if defined(CONFIG_TEDGE_CERT_RENEWAL)
	/* Renew the certificate while there is still plenty of time. */
	tedge_cert_renew_tick();
#endif
	if (tedge_auth_username() == NULL && jwt_at != 0 &&
	    k_uptime_get() - jwt_at > JWT_REFRESH_MS) {
		jwt_at = k_uptime_get(); /* one request per interval */
		(void)publish("s/uat", "", MQTT_QOS_0_AT_MOST_ONCE);
	}
	return 0;
}

static void c8y_disconnect(void)
{
	if (session_open) {
		(void)mqtt_disconnect(&client, NULL);
		session_open = false;
	}
	/* A session the broker closed just after CONNACK, three times in a
	 * row, usually means another client uses this ID. */
	if (connected_at != 0 &&
	    k_uptime_get() - connected_at < TAKEOVER_WINDOW_MS) {
		if (++takeover_strikes >= TAKEOVER_STRIKES) {
			LOG_WRN("the broker keeps closing this session moments "
				"after it connects: another client may be using "
				"the ID \"%s\"", device_id);
			takeover_strikes = 0;
		}
	} else {
		takeover_strikes = 0;
	}
	connected_at = 0;
}

static int c8y_publish_twin(const char *fragment, const char *json)
{
	return publish_twin_impl(fragment, json);
}

static const struct tedge_transport c8y_transport = {
	.connect = c8y_connect,
	.poll = c8y_poll,
	.disconnect = c8y_disconnect,
	.publish_twin = c8y_publish_twin,
};

const struct tedge_transport *tedge_transport_get(void)
{
#if defined(CONFIG_TEDGE_RESTART)
	static bool marker_loaded;

	if (!marker_loaded) {
		marker_loaded = true;
		(void)settings_load_subtree_direct(TEDGE_KEY_RESTART,
						   restart_marker_cb,
						   &restart_pending_after_boot);
		if (restart_pending_after_boot) {
			LOG_INF("a restart operation is waiting to be reported");
		}
	}
#endif
	return &c8y_transport;
}
