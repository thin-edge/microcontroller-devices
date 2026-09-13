/* SPDX-License-Identifier: Apache-2.0 */

#include "address_space.h"
#include "data_source.h"
#include "net.h"

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

/* Writable control points (in-RAM, not persisted), exposed in the application
 * namespace (ns=1) alongside the device identity and measurements. */
static int32_t g_setpoint = CONFIG_APP_SETPOINT_DEFAULT;
static bool g_running = IS_ENABLED(CONFIG_APP_RUNNING_DEFAULT);
static bool writing_back; /* guards value-callback recursion on clamp write-back */

int32_t address_space_setpoint(void)
{
	return g_setpoint;
}

bool address_space_running(void)
{
	return g_running;
}

static UA_StatusCode add_device_object(UA_Server *server)
{
	UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
	/* Display the unique per-device hostname so devices are distinguishable
	 * when browsing; keep the browse name stable for predictable navigation. */
	oAttr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)app_net_hostname());
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

/* A read-only string node exposing the device's unique id (hostname/MAC), so a
 * client can positively identify which physical device it is talking to. */
static UA_StatusCode add_device_id(UA_Server *server)
{
	UA_VariableAttributes vAttr = UA_VariableAttributes_default;
	UA_String id = UA_STRING((char *)app_net_hostname());

	UA_Variant_setScalar(&vAttr.value, &id, &UA_TYPES[UA_TYPES_STRING]);
	vAttr.displayName = UA_LOCALIZEDTEXT("en-US", "DeviceId");
	vAttr.description = UA_LOCALIZEDTEXT("en-US",
					     "Unique device id (hostname incl. MAC suffix)");
	vAttr.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
	vAttr.accessLevel = UA_ACCESSLEVELMASK_READ;

	return UA_Server_addVariableNode(
		server, UA_NODEID_STRING(APP_NS, "DeviceId"),
		device_node_id,
		UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
		UA_QUALIFIEDNAME(APP_NS, "DeviceId"),
		UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
		vAttr, NULL, NULL);
}

/* Value-callback invoked after a client writes the setpoint node. Clamp to the
 * configured range, store, log, and (if clamped) write the corrected value back. */
static void on_setpoint_write(UA_Server *server, const UA_NodeId *sid,
			      void *sctx, const UA_NodeId *nodeId, void *nctx,
			      const UA_NumericRange *range,
			      const UA_DataValue *data)
{
	ARG_UNUSED(sid); ARG_UNUSED(sctx); ARG_UNUSED(nctx); ARG_UNUSED(range);

	if (writing_back || !data->hasValue ||
	    !UA_Variant_hasScalarType(&data->value, &UA_TYPES[UA_TYPES_INT32])) {
		return;
	}

	int32_t v = *(UA_Int32 *)data->value.data;
	int32_t clamped = v;

	if (clamped < CONFIG_APP_SETPOINT_MIN) {
		clamped = CONFIG_APP_SETPOINT_MIN;
	}
	if (clamped > CONFIG_APP_SETPOINT_MAX) {
		clamped = CONFIG_APP_SETPOINT_MAX;
	}
	g_setpoint = clamped;
	LOG_INF("Setpoint written: %d (raw %d)", clamped, v);

	if (clamped != v) {
		UA_Variant val;

		UA_Variant_setScalar(&val, &clamped, &UA_TYPES[UA_TYPES_INT32]);
		writing_back = true;
		UA_Server_writeValue(server, *nodeId, val);
		writing_back = false;
	}
}

static void on_running_write(UA_Server *server, const UA_NodeId *sid,
			     void *sctx, const UA_NodeId *nodeId, void *nctx,
			     const UA_NumericRange *range,
			     const UA_DataValue *data)
{
	ARG_UNUSED(server); ARG_UNUSED(sid); ARG_UNUSED(sctx);
	ARG_UNUSED(nodeId); ARG_UNUSED(nctx); ARG_UNUSED(range);

	if (!data->hasValue ||
	    !UA_Variant_hasScalarType(&data->value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
		return;
	}
	g_running = *(UA_Boolean *)data->value.data;
	LOG_INF("Running written: %s", g_running ? "true" : "false");
}

/* Add a writable scalar node under Device (in the application namespace) and
 * register its write callback. `id` is both the string node id and browse name. */
static UA_StatusCode add_writable(UA_Server *server, const char *id,
				  const char *description,
				  const UA_DataType *type, void *initial,
				  UA_ValueCallback cb)
{
	UA_VariableAttributes vAttr = UA_VariableAttributes_default;

	UA_Variant_setScalar(&vAttr.value, initial, type);
	vAttr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)id);
	vAttr.description = UA_LOCALIZEDTEXT("en-US", (char *)description);
	vAttr.dataType = type->typeId;
	vAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

	UA_StatusCode rc = UA_Server_addVariableNode(
		server, UA_NODEID_STRING(APP_NS, (char *)id),
		device_node_id,
		UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
		UA_QUALIFIEDNAME(APP_NS, (char *)id),
		UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
		vAttr, NULL, NULL);
	if (rc != UA_STATUSCODE_GOOD) {
		return rc;
	}

	return UA_Server_setVariableNode_valueCallback(
		server, UA_NODEID_STRING(APP_NS, (char *)id), cb);
}

static void add_control_nodes(UA_Server *server)
{
	UA_Int32 sp = g_setpoint;
	UA_Boolean run = g_running;
	UA_ValueCallback sp_cb = { .onRead = NULL, .onWrite = on_setpoint_write };
	UA_ValueCallback run_cb = { .onRead = NULL, .onWrite = on_running_write };

	if (add_writable(server, "Setpoint", "Operator-settable target value",
			 &UA_TYPES[UA_TYPES_INT32], &sp, sp_cb) != UA_STATUSCODE_GOOD) {
		LOG_WRN("Failed to add writable Setpoint node");
	}
	if (add_writable(server, "Running", "Whether the simulated process is running",
			 &UA_TYPES[UA_TYPES_BOOLEAN], &run, run_cb) != UA_STATUSCODE_GOOD) {
		LOG_WRN("Failed to add writable Running node");
	}
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

	(void)add_device_id(server); /* best-effort; identity aid only */

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

	add_control_nodes(server); /* writable setpoint + enabled */

	LOG_INF("Address space ready: Device + %zu measurement(s) + controls",
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
