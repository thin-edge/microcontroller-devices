# Cumulocity tenant setup

What a Cumulocity tenant needs so that devices running a `tedge-*` image show
their measurements, their state and an editable Parameters tab. The devices
talk to the **Cumulocity MQTT Service** on thin-edge.io `te/` topics
(README, "Telemetry in Cumulocity"); on that transport Cumulocity only stores
what a **Smart Function** maps. Operations (restart, firmware, remote access,
parameters, shell) need no mapping.

The Smart Functions belong to the client and live in tiny-edge, with the
DTM definitions of the client's own parameter sets: see
[tiny-edge's `cumulocity/`](https://github.com/thin-edge/tiny-edge/tree/main/cumulocity)
for how to deploy and test them. This directory holds what the applications
here add: the DTM definitions of their parameter sets.

| What | Where | Needed for |
|---|---|---|
| Smart Functions (`tedge-zephyr-measurements`, `-twin`, `-firmware-progress`, `-events`) | tiny-edge `cumulocity/smart-functions/` | measurements, twin fragments, firmware progress and events from the device |
| `zephyr_tedge`, `zephyr_tedge_remote_access` | tiny-edge `cumulocity/dtm/` | the client's own Parameters tab entries |
| `dtm/zephyr_*.json` | here | the applications' Parameters tab entries |

## What the Smart Functions do for these applications

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

**Events.** Every `te/device/<id>///e/<type>` message becomes an event of
type `<type>` on that device, with the payload's `text` (the type when there
is none), the device's `time` when it sent one, and any other payload field
as a fragment. Without this function no event from a device on the MQTT
Service reaches the tenant. The one the firmware sends itself is
`zephyr_Identify`: an operator at a board presses `sw0` twice, and the event
shows which cloud device it is.

## Parameters tab (DTM)

The tenant needs the `dtm` and `device-parameter` microservices, and the
user registering needs `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE` and `_ADMIN`.

```sh
cumulocity/dtm/register.sh             # registers every set here not yet there
cumulocity/dtm/register.sh --replace zephyr_modbus_telemetry   # after a declaration changed
```

Register the client's own sets with the same script in tiny-edge.

| Identifier | Declared by | Parameters |
|---|---|---|
| `zephyr_modbus_telemetry` | `modbus-server` | `interval_s` (5–3600 s), `telemetry`, `measurement_type` (≤ 31 chars) |
| `zephyr_modbus_control` | `modbus-server` | `running`, `mode` (off/auto/manual), `speed_setpoint` (0–100 %), `local_writes` (whether Modbus clients may change the three) |
| `zephyr_opcua_telemetry` | `opcua-server` | `interval_s` (5–3600 s), `telemetry` |
| `zephyr_snmp_telemetry` | `snmp-agent` | `interval_s` (5–3600 s), `telemetry` |
| `zephyr_tedge` | tiny-edge, every image | `log_level` (off/err/wrn/inf/dbg), `health_interval_s` (0–86400 s), `required_interval_min` (0–1440 min) |
| `zephyr_tedge_remote_access` | tiny-edge, images with remote access | `remote_access` |

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
ESP32-S3 carry the shell command (`tiny-edge/ports/zephyr/profiles/extras/shell-diagnostics.conf`)
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
