## 1. Prove it works

- [x] 1.1 Receive one operation with its id on `devicecontrol/notifications` on the MQTT Service (9883), from the device, before writing the rest
- [x] 1.2 The same on Core MQTT (8883)

## 2. Receiving

- [x] 2.1 Subscribe to `devicecontrol/notifications` in place of `s/ds`; Kconfig `TEDGE_C8Y_OPERATION_JSON` and `TEDGE_C8Y_PAYLOAD_BYTES` (D1, D7)
- [x] 2.2 `tedge_json_value()`: a value that may be a number as well as a string (D3)
- [x] 2.3 Turn an operation's JSON into the line the handlers already parse, with the id where the serial was, quoting what needs it (D2)
- [x] 2.4 Log the fragment and the id, never the payload (D3)

## 3. Carrying the id

- [x] 3.1 The dispatcher keeps the id of the operation in flight
- [x] 3.2 `504`/`505`/`506` in place of `501`/`502`/`503`, including the unsupported-operation path (D4)
- [x] 3.3 The restart and the firmware update keep their id across the reboot (D5)
- [x] 3.4 The queue keeps working, with its reason restated as a resource bound (D6)

## 4. Tests

- [x] 4.1 Unit tests: the JSON-to-line translation for each operation, including a comma in a command and a numeric port
- [x] 4.2 Unit tests: `tedge_json_value()` for strings, numbers and missing keys
- [x] 4.3 Kconfig cases: the JSON path on by default; the static path still builds

## 5. Hardware verification

- [x] 5.1 Every operation still works end to end: restart, firmware update, remote access, log upload, shell command
- [x] 5.2 Two operations created together each get their own result
- [x] 5.3 A stale EXECUTING operation does not absorb a later result
- [x] 5.4 A restart is completed for the operation that asked for it
- [x] 5.5 The static path (feature off) still behaves as before
- [x] 5.6 Footprint row

## 6. Wrap-up

- [x] 6.1 README: what the collection is, why the id matters, and the option to go back
- [x] 6.2 design.md results; archive
