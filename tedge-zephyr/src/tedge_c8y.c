/* SPDX-License-Identifier: Apache-2.0
 *
 * The Cumulocity transport: the MQTT session (MQTT Service on 9883 with a
 * certificate, or Core MQTT on 8883), the SmartREST dispatcher, the JWT and
 * the twin/inventory publishing.
 *
 * Filled in by tasks 2.x of c8y-direct-core; the core already drives it.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL);

static int c8y_connect(void)
{
	LOG_ERR("the Cumulocity transport is not implemented yet");
	return -ENOTSUP;
}

static int c8y_poll(int timeout_ms)
{
	k_msleep(timeout_ms);
	return -ENOTCONN;
}

static void c8y_disconnect(void)
{
}

static int c8y_publish_twin(const char *fragment, const char *json)
{
	ARG_UNUSED(fragment);
	ARG_UNUSED(json);
	return -ENOTCONN;
}

static const struct tedge_transport c8y_transport = {
	.connect = c8y_connect,
	.poll = c8y_poll,
	.disconnect = c8y_disconnect,
	.publish_twin = c8y_publish_twin,
};

const struct tedge_transport *tedge_transport_get(void)
{
	return &c8y_transport;
}
