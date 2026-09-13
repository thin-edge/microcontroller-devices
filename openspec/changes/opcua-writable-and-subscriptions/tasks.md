## Status

**Writable data points: implemented and verified on hardware (ESP32-WROOM).** A
client writes `setpoint`/`enabled`, reads them back, out-of-range setpoint is
clamped, and writing a read-only measurement is rejected (`BadNotWritable`).

**Subscriptions: gated off by default.** They require `UA_NAMESPACE_ZERO=REDUCED`
(MINIMAL silently disables them), and REDUCED ns0 OOMs at namespace init on the
WROOM's ~68 KB heap. So the committed amalgamation is the **minimal** profile
(subscriptions off, writes on) and the WROOM runs reads+writes. Subscriptions
are an opt-in for higher-RAM boards via `scripts/regen-open62541.sh <src> <lvl>
reduced` — build-verified (compiles, subscriptions enabled) but not
runtime-verified (the WROOM can't fit them; the PSRAM S2 has the RAM but its
Wi-Fi wasn't holding a connection this session).

## 1. open62541 build — enable subscriptions

- [x] 1.1 Add `-DUA_ENABLE_SUBSCRIPTIONS=ON` to `scripts/regen-open62541.sh` and regenerate the vendored amalgamation (re-apply the Zephyr patches)
- [x] 1.2 Build `native_sim` with subscriptions and confirm it compiles/links and the server still starts
- [x] 1.3 Build ESP32-WROOM and record flash/RAM; check the server starts (no `BadOutOfMemory`) after Wi-Fi

## 2. Cap subscription resources (capability: opcua-server)

- [x] 2.1 Add Kconfig for max subscriptions, max monitored items per subscription, and publish/notification queue caps (small defaults)
- [x] 2.2 Apply the caps to the server config at startup
- [x] 2.3 If the WROOM cannot fit subscriptions after capping, gate `UA_ENABLE_SUBSCRIPTIONS`/`APP_OPCUA_SUBSCRIPTIONS` behind a board/Kconfig profile (S2 on, WROOM off) and document it

## 3. Writable data points (capability: device-data-model)

- [x] 3.1 Add a `setpoint` (Double) node under Device with `CurrentRead | CurrentWrite` and a Kconfig default + documented range
- [x] 3.2 Add an `enabled` (Boolean) node under Device with `CurrentRead | CurrentWrite` and a Kconfig default
- [x] 3.3 Register write value-callbacks that validate/clamp the value and mirror it into app state
- [ ] 3.4 React to writes: log the new value and reflect it on the TFT status display where present

## 4. Verification

- [ ] 4.1 `native_sim`/hardware: client creates a subscription + monitored item on `temperature` and receives change notifications as the sampler updates it
- [x] 4.2 Client writes `setpoint` and `enabled`; reads back the written values; firmware logs the change
- [x] 4.3 Confirm writing a read-only measurement node is rejected (Bad status)
- [ ] 4.4 Confirm resource caps: excess subscriptions/monitored items are rejected gracefully; server stays stable
- [ ] 4.5 Record final flash/RAM per board and update the README/board notes (which boards have subscriptions enabled)

## 5. Documentation

- [ ] 5.1 Update README: subscriptions + writable `setpoint`/`enabled` (with range), example client snippet, and per-board subscription availability
- [ ] 5.2 Update `scripts/regen-open62541.sh` header/notes for the subscriptions flag
