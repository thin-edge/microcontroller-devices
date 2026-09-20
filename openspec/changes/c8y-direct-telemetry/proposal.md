## Why

A device exists to report something. Everything the client has built so far —
onboarding, the connection, remote access, firmware update, renewal — is
scaffolding around that. The API for it is already declared in
`include/tedge/tedge.h` and returns `-ENOTSUP`: applications can integrate
the client but cannot yet send a single measurement through it.

The shape is settled. The spikes published free-form telemetry on
thin-edge.io's `te/` topics and the tenant owner mapped it to Cumulocity
measurements with a Smart Function; the same messages suit a thin-edge.io
gateway later. Two things were left open and belong here: telemetry must
carry its own **timestamp**, so a reading buffered while offline keeps the
time it was taken (spike problem P4), and the client needs an **alarm** API,
which certificate renewal already wants and publishes by hand today.

## What Changes

- **`CONFIG_TEDGE_TELEMETRY` becomes real:** `tedge_publish_measurement()`,
  `tedge_publish_event()`, `tedge_raise_alarm()` and `tedge_clear_alarm()`
  do what their documentation says, from any thread.
- **Every message carries a timestamp**, taken when the application
  publishes, not when the message leaves. An application may pass its own.
- **Messages survive a short outage:** a bounded buffer in the module's heap
  holds what cannot be sent yet and flushes it on reconnect, oldest first.
  When it is full, the oldest measurement is dropped and counted, and the
  count is reported, so data loss is visible rather than silent.
- **Both transports:** free-form `te/` topics on the MQTT Service, SmartREST
  (`200`, `400`, `301`/`306`) on Core MQTT, from the same API call.
- **`CONFIG_TEDGE_HEALTH` becomes real:** the client publishes its own vital
  signs — uptime, free heap, the reason for the last reset, and the Wi-Fi
  signal where the application offers it — on an interval, so a device that
  is degrading can be spotted before it fails.
- **Certificate renewal stops hand-rolling its alarm** and uses the API.

## Non-goals

- Storing telemetry across a reboot. The buffer is RAM; a device that
  reboots loses what it had not sent. Persisting it is a separate decision
  about flash wear.
- Aggregation, batching into one message, or any sampling logic: the
  application decides what and when to measure.
- Child devices and services other than the client's own.

## Resource constraints

| Item | Cost |
|---|---|
| Text | small: JSON building and the topic rules; the transport is already there |
| RAM | the buffer, `TEDGE_TELEMETRY_BUFFER_BYTES` (default 2048) from the module heap, plus one message being built |
| Network | one publish per call, QoS 0 for measurements (a lost reading is a lost reading), QoS 1 for events and alarms |
| Time | building a message is microseconds; nothing blocks the caller |

## Capabilities

### New Capabilities

- `tedge-telemetry`: what an application can send, when it is sent, what
  happens to it while the device is offline, and what the client reports
  about itself.

### Modified Capabilities

- `device-management-features`: telemetry and device health stop being
  unimplemented features.
- `tedge-client-module`: the telemetry hooks in the integration contract
  become requirements with scenarios (the module still samples nothing by
  itself).

## Impact

- `tedge-zephyr/src/`: a new `tedge_telemetry.c` (the API, the buffer, the
  JSON) and `tedge_health.c` (the client's own vital signs), with the
  transport picking the topic and encoding.
- `tedge_cert_renew.c` switches to `tedge_raise_alarm()`.
- The Modbus application publishes its pump simulation through the API, so
  the repository shows an application using it end to end.
- Cumulocity: the tenant's Smart Functions map the `te/` topics, as they
  already do for the twin.
