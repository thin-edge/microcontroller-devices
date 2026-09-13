## Why

Phase 1 delivered a read-only OPC-UA server: clients must poll, and no value can
be sent back to the device. Real collectors want change-driven updates
(subscriptions/monitored items) instead of polling, and many use cases need to
push a value to the device (e.g. a setpoint or an enable flag). This extends the
OPC-UA server to cover those two capabilities, which were explicit Phase 1
non-goals.

## What Changes

- **Subscriptions / monitored items**: re-enable `UA_ENABLE_SUBSCRIPTIONS` in the
  vendored open62541 build so clients can subscribe to nodes and receive
  value-change notifications, with the sampling/publishing counts capped to fit
  MCU RAM.
- **Writable data points**: add client-writable nodes to the device data model —
  a numeric `setpoint` and a boolean `enabled` flag — with `CurrentWrite` access
  and a write path so the firmware reacts to writes (logged, reflected on the
  status display where present, and readable back).
- **Resource guardrails**: cap max subscriptions, monitored items and publish
  queue sizes; measure RAM on the ESP32-WROOM and document the minimum viable
  board profile (the S2/PSRAM path has more headroom).

**Phase**: Phase 1 (extension). **Protocol**: OPC-UA (server role — adds Write
service + Subscription/MonitoredItem services). **Boards**: `esp32_devkitc`
(ESP32-WROOM), `adafruit_feather_esp32s2_tft` (ESP32-S2), `native_sim`.

### Non-goals

- OPC-UA method calls, historical access, events/alarms, and PubSub — still out.
- Security/encryption for writes (writes remain on the basic unsecured endpoint;
  a proper security profile is tracked separately).
- Persisting written values across reboots (in-RAM only for now).
- Actuating real hardware from the writable points — the firmware reacts by
  updating state/logs/display; concrete actuator bindings can follow.

### Resource constraints

Subscriptions add per-subscription and per-monitored-item allocations plus a
publish queue, on top of the already-tight open62541 footprint (~68 KB free heap
on the WROOM after Wi-Fi). Counts must be capped (few subscriptions / monitored
items) and RAM measured; if it does not fit the WROOM, subscriptions may be
gated to the PSRAM-equipped S2 while writes (cheap) stay on all boards.

## Capabilities

### New Capabilities
<!-- None — extends existing capabilities. -->

### Modified Capabilities
- `opcua-server`: add Subscription/MonitoredItem support (value-change
  notifications) and the Write service, with capped resource limits.
- `device-data-model`: add writable data points (`setpoint`, `enabled`) with
  write handling, alongside the existing read-only measurements.

## Impact

- **Build**: regenerate the vendored open62541 amalgamation with
  `UA_ENABLE_SUBSCRIPTIONS=ON` (update `scripts/regen-open62541.sh`); slightly
  larger flash/RAM.
- **Code**: add writable nodes + a write value-callback in the address-space
  module; wire written state into the app (logging/display/data-source).
- **Config**: Kconfig for sampling/publish caps and initial setpoint/enabled
  defaults.
- **Systems**: no cloud/thin-edge.io involvement; still a LAN OPC-UA server.
