/* SPDX-License-Identifier: Apache-2.0 */

#include "address_space.h"
#include "data_source.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <open62541.h>

LOG_MODULE_REGISTER(app_addrspace, CONFIG_LOG_DEFAULT_LEVEL);

/* Application namespace index (namespace 0 is the OPC-UA standard space). */
#define APP_NS 1

/* Node id of the Device object; measurement variables hang off it. */
static UA_NodeId device_node_id;

/* One string node id per measurement, so update() can write values back. */
#define MAX_MEASUREMENTS 16
static UA_NodeId measurement_ids[MAX_MEASUREMENTS];
static size_t measurement_count;

static UA_StatusCode add_device_object(UA_Server *server)
{
	UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
	oAttr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)CONFIG_APP_DEVICE_NAME);
	oAttr.description = UA_LOCALIZEDTEXT("en-US",
					    "Zephyr OPC-UA device (Phase 1)");

	device_node_id = UA_NODEID_STRING_ALLOC(APP_NS, "Device");

	return UA_Server_addObjectNode(
		server, device_node_id,
		UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
		UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
		UA_QUALIFIEDNAME(APP_NS, (char *)CONFIG_APP_DEVICE_NAME),
		UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
		oAttr, NULL, NULL);
}

static UA_StatusCode add_measurement_variable(UA_Server *server, size_t index)
{
	const struct data_measurement *m = data_source_descriptor(index);

	if (m == NULL) {
		return UA_STATUSCODE_BADOUTOFRANGE;
	}

	UA_VariableAttributes vAttr = UA_VariableAttributes_default;
	UA_Double initial = data_source_sample(index);
	UA_Variant_setScalar(&vAttr.value, &initial, &UA_TYPES[UA_TYPES_DOUBLE]);
	vAttr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)m->name);
	vAttr.description = UA_LOCALIZEDTEXT("en-US", (char *)m->unit);
	vAttr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
	vAttr.accessLevel = UA_ACCESSLEVELMASK_READ;

	measurement_ids[index] = UA_NODEID_STRING_ALLOC(APP_NS, (char *)m->name);

	return UA_Server_addVariableNode(
		server, measurement_ids[index],
		device_node_id,
		UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
		UA_QUALIFIEDNAME(APP_NS, (char *)m->name),
		UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
		vAttr, NULL, NULL);
}

UA_StatusCode address_space_setup(UA_Server *server)
{
	UA_StatusCode rc;

	data_source_init();

	rc = add_device_object(server);
	if (rc != UA_STATUSCODE_GOOD) {
		LOG_ERR("Failed to add Device object: %s",
			UA_StatusCode_name(rc));
		return rc;
	}

	measurement_count = data_source_count();
	if (measurement_count > MAX_MEASUREMENTS) {
		LOG_WRN("Clamping measurements %zu -> %d",
			measurement_count, MAX_MEASUREMENTS);
		measurement_count = MAX_MEASUREMENTS;
	}

	for (size_t i = 0; i < measurement_count; i++) {
		rc = add_measurement_variable(server, i);
		if (rc != UA_STATUSCODE_GOOD) {
			LOG_ERR("Failed to add measurement %zu: %s", i,
				UA_StatusCode_name(rc));
			return rc;
		}
	}

	LOG_INF("Address space ready: Device + %zu measurement(s)",
		measurement_count);
	return UA_STATUSCODE_GOOD;
}

void address_space_update(UA_Server *server)
{
	for (size_t i = 0; i < measurement_count; i++) {
		UA_Double value = data_source_sample(i);
		UA_Variant v;
		UA_Variant_init(&v);
		UA_Variant_setScalar(&v, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
		UA_Server_writeValue(server, measurement_ids[i], v);
	}
}
