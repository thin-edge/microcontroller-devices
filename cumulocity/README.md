# Cumulocity tenant setup

What a Cumulocity tenant needs so that devices running a `tedge-*` image show
their measurements, their state and an editable Parameters tab. The devices
talk to the **Cumulocity MQTT Service** on thin-edge.io `te/` topics
(README, "Telemetry in Cumulocity"); on that transport Cumulocity only stores
what a **Smart Function** maps. Operations (restart, firmware, remote access,
parameters, shell) need no mapping.

| Directory | What it is | Needed for |
|---|---|---|
| `smart-functions/tedge-zephyr-measurements/` | `te/device/*///m/*` → measurements | any measurement, including `tedge_health` |
| `smart-functions/tedge-zephyr-twin/` | `te/device/*///twin/*` → managed-object fragments | `remoteAccess`, `tedge_Agent`, and the current parameter values the Parameters tab shows |
| `smart-functions/tedge-zephyr-firmware-progress/` | `te/device/*///progress/firmware` → `c8y_FirmwareDownload` events | download progress while a firmware update runs (optional) |
| `dtm/*.json` | Digital Twin Manager property definitions | the Parameters tab |

## Smart Functions

Each directory is a Data Preparation rule package: `data-prep.yaml` (the
topic pattern), the JavaScript, and `tests/*.yaml`. Create a Smart Function
in the Data Preparation UI with the topic and the JavaScript, or deploy the
package:

```sh
cd cumulocity/smart-functions/tedge-zephyr-twin
# COPYFILE_DISABLE: macOS tar otherwise adds ._* files the service rejects
COPYFILE_DISABLE=1 tar czf /tmp/rule.tar.gz data-prep.yaml twin.js tests
# The archive must be the raw request body: `c8y api --file` sends a
# multipart form, which the service answers with 400.
curl -X PUT -H "Authorization: Bearer $C8Y_TOKEN" \
  -H "Content-Type: application/gzip" --data-binary @/tmp/rule.tar.gz \
  "$C8Y_HOST/service/dataprep/v1/rules/tedge-zephyr-twin/deployed"
```

`smart-functions/test.py` runs every rule's tests in a tenant's Data
Preparation runtime (`/service/dataprep/v1/run-tests`) without deploying
anything; run it after changing a function.

**Measurements.** Known types get a Cumulocity type, one fragment and named
series with units (the device sends none):

| Topic type | Cumulocity type | Fragment: series (unit) |
|---|---|---|
| `pump` | `c8y_Pump` | `pump`: flow (l/min), pressure (bar), motorTemperature (C), speed (rpm), vibration (mm/s), runHours (h) |
| `server` | `c8y_Server` | `server`: temperature (C), humidity (%RH), pressure (hPa) |
| `agent` | `c8y_Agent` | `agent`: uptime (s) |
| `device` | `c8y_Device` | `device`: uptime (s), freeHeap (bytes), rssi (dBm) |
| `tedge_health` | `c8y_TedgeHealth` | `tedge_Health`: uptime (s), freeHeap (bytes), droppedMessages, resetCause |

Any other type — an operator can rename the Modbus one — is kept with the
type as fragment and the keys as series, rather than dropped. The device's
own timestamp is used when it sends one.

**Twin.** Every `te/device/<id>///twin/<name>` message is written unchanged
under a fragment named `<name>`, except `tedge_RemoteAccess`, which becomes
`remoteAccess`. That is what puts a parameter set's current values
(`zephyr_modbus_telemetry`, `zephyr_modbus_control`, `zephyr_opcua_telemetry`,
`zephyr_snmp_telemetry`, `zephyr_tedge`) on the managed object, where the Parameters tab
reads them. It replaces a rule that mapped only `tedge_RemoteAccess`; do not
deploy both.

## Parameters tab (DTM)

The tenant needs the `dtm` and `device-parameter` microservices, and the
user registering needs `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE` and `_ADMIN`.

```sh
cumulocity/dtm/register.sh             # registers every set not yet there
cumulocity/dtm/register.sh --replace zephyr_modbus_telemetry   # after a declaration changed
```

| Identifier | Declared by | Parameters |
|---|---|---|
| `zephyr_modbus_telemetry` | `modbus-server` | `interval_s` (5–3600 s), `telemetry`, `measurement_type` (≤ 31 chars) |
| `zephyr_modbus_control` | `modbus-server` | `running`, `mode` (off/auto/manual), `speed_setpoint` (0–100 %), `local_writes` (whether Modbus clients may change the three) |
| `zephyr_opcua_telemetry` | `opcua-server` | `interval_s` (5–3600 s), `telemetry` |
| `zephyr_snmp_telemetry` | `snmp-agent` | `interval_s` (5–3600 s), `telemetry` |
| `zephyr_tedge` | tedge-zephyr, every image | `log_level` (off/err/wrn/inf/dbg), `health_interval_s` (0–86400 s), `required_interval_min` (0–1440 min) |
| `zephyr_tedge_remote_access` | tedge-zephyr, images with remote access | `remote_access` |

Every identifier starts with `zephyr_` and, for an application's own set,
the application, so it cannot collide with a definition someone else
registers for another device (tedge-dot, for one, registers
`zephyr_modbus_pump_control_parameters` for the same pump, written over
Modbus). Each file's `tags` name the Cumulocity device types that declare
the set. `zephyr_modbus_control.local_writes` set to false refuses *every*
Modbus write to the controls, tedge-dot's included.

The identifier is the set's name, which is also the twin fragment and the
suffix of the `c8y_ParameterUpdate_<set>` operation, so all three have to
agree. The files were printed by the firmware
(`tedge params schema <set>`, from a build with a serial shell and
`CONFIG_TEDGE_PARAMETERS_SCHEMA`), so they match what the device validates.
A device refuses a change that names a parameter it does not declare, and
the Parameters tab sends the whole set, so every field in a definition has
to be declared by every device that declares the set. That is why
`remote_access` is a set of its own, `zephyr_tedge_remote_access`, declared
only by images with remote access: inside `zephyr_tedge` it made every
change to that set fail on the WROOM and the ESP32-CAM.

The tab appears on a device whose managed object carries the set's fragment,
which the twin Smart Function writes when the device connects. A change the
device refuses (out of range, unknown name) fails the operation with the
reason, and nothing is applied.

## Shell diagnostics

The `tedge-full` Modbus and agent images of the ESP32-S3-DevKitC and the QT Py
ESP32-S3 carry the shell command (`lib/common/tedge-boards/extras/shell-diagnostics.conf`)
for general checks from the device's **Shell** tab. Send `help` to see what
the device runs:

| Command | Answers |
|---|---|
| `kernel uptime`, `kernel version` | how long it has run, what it runs |
| `net iface`, `net conn` | addresses, gateway, open sockets |
| `wifi status` | SSID, RSSI, channel, security |
| `tedge params list` | the parameter sets it declares |
| `tedge diag` | the client's state and uptime |

`net ping` is not offered: Zephyr prints the replies after the command
returns, so the cloud would only ever see the `PING <host>` line.

Anything else is refused with the reason. Output travels in one SmartREST
field, so a long answer is cut. The other boards' images leave it out: on
the ESP32-C6 its ~16 KB of RAM left too little for the Wi-Fi driver and the
firmware download (DEVICES.md, "Shell diagnostics measured").
