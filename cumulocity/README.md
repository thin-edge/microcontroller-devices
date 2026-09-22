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
tar czf /tmp/rule.tar.gz data-prep.yaml twin.js tests
c8y api PUT /service/dataprep/v1/rules/tedge-zephyr-twin/deployed \
  --file /tmp/rule.tar.gz --contentType application/gzip
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
`remoteAccess`. That is what puts a parameter set's current values (`pump`,
`server`, `agent`, `tedge`) on the managed object, where the Parameters tab
reads them. It replaces a rule that mapped only `tedge_RemoteAccess`; do not
deploy both.

## Parameters tab (DTM)

The tenant needs the `dtm` and `device-parameter` microservices, and the
user registering needs `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE` and `_ADMIN`.

```sh
cumulocity/dtm/register.sh             # registers every set not yet there
cumulocity/dtm/register.sh --replace pump   # after a declaration changed
```

| Identifier | Declared by | Parameters |
|---|---|---|
| `pump` | `modbus-server` | `interval_s` (5–3600 s), `telemetry`, `measurement_type` (≤ 31 chars) |
| `server` | `opcua-server` | `interval_s` (5–3600 s), `telemetry` |
| `agent` | `snmp-agent` | `interval_s` (5–3600 s), `telemetry` |
| `tedge` | tedge-zephyr, every image | `log_level` (off/err/wrn/inf/dbg), `health_interval_s` (0–86400 s), `required_interval_min` (0–1440 min), `remote_access` |

The identifier is the set's name, which is also the twin fragment and the
suffix of the `c8y_ParameterUpdate_<set>` operation, so all three have to
agree. The files were printed by the firmware
(`tedge params schema <set>`, from a build with a serial shell and
`CONFIG_TEDGE_PARAMETERS_SCHEMA`), so they match what the device validates.
`tedge` lists every parameter the client can offer; an image without remote
access declares no `remote_access` and refuses a change that includes it.

The tab appears on a device whose managed object carries the set's fragment,
which the twin Smart Function writes when the device connects. A change the
device refuses (out of range, unknown name) fails the operation with the
reason, and nothing is applied.

## Shell diagnostics

The ESP32-S3-DevKitC's `tedge-full` Modbus and agent images carry the shell
command (`lib/common/tedge-boards/extras/shell-diagnostics.conf`)
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
