/* SPDX-License-Identifier: Apache-2.0
 *
 * Protocol-frontend TEMPLATE (skeleton, not a working protocol). Shows how a
 * frontend reads the shared device data model from lib/common. Replace the body
 * with a real protocol server/agent.
 */

#include "template_frontend.h"

#include "data_source.h"
#include "controls.h"
#include "identity.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(template_frontend, CONFIG_LOG_DEFAULT_LEVEL);

int template_frontend_start(void)
{
	/* Identity is available from lib/common. */
	LOG_INF("template frontend: device=%s firmware=%s %s",
		app_identity_device_id(), app_identity_firmware_name(),
		app_identity_firmware_version());

	/* Measurements come from the shared data source. */
	for (size_t i = 0; i < data_source_count(); i++) {
		const struct data_measurement *m = data_source_descriptor(i);

		LOG_INF("  measurement %zu: %s (%s) = %f", i,
			m ? m->name : "?", m ? m->unit : "?",
			data_source_sample(i));
	}

	/* Writable control points are read/written through lib/common (the setter
	 * clamps). A real frontend would map protocol writes onto these. */
	LOG_INF("  setpoint=%d running=%d", app_control_setpoint(),
		app_control_running());

	/* A real frontend would bind its socket/bus and run its event loop here. */
	return 0;
}
