## 1. The telemetry path

- [x] 1.1 `tedge_telemetry.c`: the byte-ring buffer in the module heap, with events and alarms ahead of measurements and the oldest measurement dropped when full (D2)
- [x] 1.2 Build each message at the point of call, with a timestamp (the application's or the client's), and the "no clock yet" case (D3)
- [x] 1.3 `tedge_publish_measurement()`, `tedge_publish_event()`, `tedge_raise_alarm()`, `tedge_clear_alarm()`: queue and return, safe from any thread (D1)
- [x] 1.4 The client thread drains the queue while connected, oldest first
- [x] 1.5 Transport encoding: `te/` topics on the MQTT Service, SmartREST on Core MQTT, QoS 0 for measurements and QoS 1 for events and alarms (D4)
- [x] 1.6 Kconfig: `TEDGE_TELEMETRY_BUFFER_BYTES`; drop the experimental gate for telemetry

## 2. The client's own health

- [x] 2.1 `tedge_health.c`: uptime, free module heap, dropped messages and the reset reason from `hwinfo`, every `TEDGE_HEALTH_INTERVAL_S` (D5)
- [x] 2.2 Kconfig: `TEDGE_HEALTH_INTERVAL_S`; drop the experimental gate for health

## 3. Use it

- [x] 3.1 Certificate renewal raises its alarm through the API, falling back to SmartREST when telemetry is not built in (D6)
- [x] 3.2 The Modbus application publishes its simulation through the API, so the repository has a working example

## 4. Tests

- [x] 4.1 Unit tests: the buffer (fits, full, oldest dropped, events ahead of measurements, the drop counter)
- [x] 4.2 Unit tests: the message builders (measurement with several series, event, alarm severities, with and without a timestamp)
- [x] 4.3 Kconfig cases: telemetry and health selectable; the API absent when they are off

## 5. Hardware verification

- [x] 5.1 C6: measurements from the Modbus application appear in Cumulocity (natively on Core MQTT; on the MQTT Service the `te/` payloads were captured on the device and await the tenant's Smart Function, which the tenant owner writes)
- [x] 5.2 An event and an alarm appear, and the alarm clears
- [x] 5.3 Timestamps: publish while disconnected, reconnect, and confirm the cloud records the time of the reading
- [x] 5.4 The buffer: publish faster than the link for a while and confirm the device keeps running and reports what it dropped
- [x] 5.5 Health measurements arrive on their interval
- [x] 5.6 Core MQTT: the same application reports the same measurements as SmartREST
- [x] 5.7 Footprint row with telemetry and health enabled

## 6. Wrap-up

- [x] 6.1 README: the telemetry API, what the payloads look like, the buffer's limits, and what belongs to the application rather than the client
- [x] 6.2 Profiles: telemetry and health in `full.conf` and `minimal.conf`; record the results in design.md and update the roadmap
