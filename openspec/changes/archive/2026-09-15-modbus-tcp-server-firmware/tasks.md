## 1. Pluggable simulation layer in lib/common (OPC-UA left unchanged)

- [x] 1.1 Add a Kconfig `choice` in `lib/common/Kconfig`: `APP_SIM_ENVIRONMENT` (default) | `APP_SIM_PUMP`; add `mode` control config (default) additively
- [x] 1.2 Refactor `lib/common/data_source.*` so the current temperature/humidity/pressure model becomes the **environment simulation** compiled under `APP_SIM_ENVIRONMENT` — identical behavior (no output change)
- [x] 1.3 Extend `lib/common/controls.*` with `mode` (int enum, clamp 0–2) additively; keep `setpoint`/`running` unchanged (environment sim ignores `mode`)
- [x] 1.4 Add the **pump simulation** under `APP_SIM_PUMP` (e.g. `sim_pump.c`): a timer/work item stepped every `CONFIG_APP_SAMPLE_INTERVAL_MS` reading controls and integrating state — commanded speed from running/mode/speed_setpoint, speed ramp (inertia), affinity laws (flow ∝ speed, pressure ∝ speed²), rpm/vibration from speed, first-order thermal model, run_hours accrual while running, over-temp fault (`app_sim_fault()`); deterministic jitter; provides measurements `flow_lpm`/`pressure_bar`/`motor_temp_c`/`rpm`/`vibration_mms`/`run_hours` via the data_source interface
- [x] 1.5 Build `apps/opcua-server` (`APP_SIM_ENVIRONMENT`) for `native_sim`; confirm the OPC-UA node set is unchanged (regression: environment sim outputs identical) — no OPC-UA code changes
- [x] 1.6 Build the pump sim path (`APP_SIM_PUMP`) for `native_sim` (build-only)

## 2. Scaffold the modbus frontend + app

- [x] 2.1 Create `lib/modbus/` (zephyr/module.yml, Kconfig, CMakeLists.txt) depending on lib/common
- [x] 2.2 `lib/modbus/Kconfig`: `APP_MODBUS_SERVER` (default y), `APP_MODBUS_PORT` (default 502), `APP_MODBUS_UNIT_ID` (default 1); select `MODBUS`/`MODBUS_RAW_ADU`
- [x] 2.3 Create `apps/modbus-server/` (CMakeLists composing lib/common + lib/modbus, Kconfig sourcing Kconfig.zephyr, prj.conf with `CONFIG_APP_FIRMWARE_NAME="zephyr-modbus-server"` and `CONFIG_APP_SIM_PUMP=y`, VERSION)
- [x] 2.4 `apps/modbus-server/src/main.c`: bring up lib/common (display + net), wait for connectivity, call `modbus_server_start()`

## 3. Modbus server frontend (device side)

- [x] 3.1 TCP listener thread on `CONFIG_APP_MODBUS_PORT` (accept one client; read MBAP header + PDU; retry-not-fatal on transient errors; replace stale client on new accept) — mirror `samples/subsys/modbus/tcp_server`
- [x] 3.2 Raw-ADU server: `modbus_init_server` (`MODBUS_MODE_RAW`), `raw_tx_cb` → socket write, `modbus_raw_submit_rx` on receive, MBAP via `modbus_raw_get/put_header`
- [x] 3.3 Implement `struct modbus_user_callbacks` for the register map: input-reg read (IR0..4 scaled ints, IR10..11 run_hours uint32, IR20..25 float pairs), holding-reg read/write (HR0 speed_setpoint, HR1 mode), coil read/write (Coil0 running), discrete-input read (DI0..2); illegal-data-address for unmapped
- [x] 3.4 Route writes through `app_control_set_*` (shared clamps); reads from `data_source_*`/`app_control_*`; DI2 from `app_net_is_connected()`; refresh measurement values on the common sample interval
- [x] 3.5 (Optional) advertise `_modbus._tcp` via DNS-SD alongside the unique hostname
- [x] 3.6 Build `apps/modbus-server` for `native_sim/native/64` (build-only; NSOS TCP caveat)

## 4. Board bring-up (device side)

- [x] 4.1 Add `apps/modbus-server/boards/esp32_devkitc_esp32_procpu.conf` (Wi-Fi + IPv4/TCP + mDNS + modest net tuning; no open62541 pressure)
- [x] 4.2 Build for `esp32_devkitc/esp32/procpu`, flash an ESP32-WROOM; confirm Wi-Fi join + listening on 502
- [x] 4.3 Add `apps/modbus-server/boards/adafruit_qt_py_esp32s3_esp32s3_procpu.conf` (+ &wifi overlay); build for the S3

## 5. Client/master verification (collector side)

- [x] 5.1 Read Input Registers with a client (pymodbus/`mbpoll`): IR0..4 scaled ints match live flow/pressure/motor-temp/rpm/vibration and change over time
- [x] 5.2 Read IR20..25 as IEEE-754 float pairs (big-endian) and confirm they equal the measurements
- [x] 5.3 Read IR10..11 as a 32-bit counter; confirm run_hours is non-decreasing across reads
- [x] 5.4 Read Discrete Inputs DI0..2 (running mirror, fault sim, network connected)
- [x] 5.5 Write Holding Register HR0 (speed_setpoint) in range and above 100; confirm clamp on read-back; write HR1 (mode) and confirm 0–2 clamp
- [x] 5.6 Toggle Coil0 (FC05); confirm running + DI0 mirror update
- [x] 5.7 Read an unmapped address; confirm illegal-data-address exception (not a crash)
- [x] 5.8 Disconnect and reconnect the client; confirm the server keeps serving without a reboot
- [x] 5.9 Simulation coupling over Modbus: set Coil0=on + HR0 to a high speed (manual mode) and observe IR3/IR0/IR1 (rpm/flow/pressure) rise and motor_temp warm; set Coil0=off and observe them fall toward zero and run_hours (IR10..11) stop advancing

## 6. Documentation

- [x] 6.1 Add modbus-server to README targets + build/flash section + the register map table
- [x] 6.2 Add a client test recipe (pymodbus snippet / `mbpoll` commands) for each object type
- [x] 6.3 Update README/SCOPE for the pluggable simulation layer (how to select a sim per app; environment vs pump) and the second protocol frontend (OPC-UA unchanged); commit
