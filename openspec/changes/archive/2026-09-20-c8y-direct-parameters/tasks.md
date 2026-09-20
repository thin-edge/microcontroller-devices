## 1. Settle the mechanism

- [x] 1.1 Confirm the tenant has Parameter Update (Digital Twin Manager and the device-parameter microservice), and capture what a `c8y_ParameterUpdate` operation actually carries, before writing device code
- [x] 1.2 Register one hand-written schema in the tenant and change a value against a mock device, so the round trip is understood end to end — **done with a caveat**: the schema was registered and the round trip was driven end to end against real hardware rather than a mock (section 8), which is stronger. What could not be exercised is the *operator UI's* half: `device-parameter` is not subscribed by the tenant, so nothing turns an edit into an operation, and whether the UI sends the whole set or only the changed values is still open. See findings-mechanism.md.

## 2. The declaration

- [x] 2.1 The public API: `struct tedge_parameter`, the `TEDGE_PARAM_*` macros, `tedge_declare_parameters()` and the change hook (D1)
- [x] 2.2 Remove the configuration-file API from the header, with a line saying what replaced it (D7)
- [x] 2.3 Kconfig: `TEDGE_PARAMETERS`, `TEDGE_PARAMETERS_MAX`, `TEDGE_PARAMETERS_SCHEMA`; drop `TEDGE_CONFIG` and its experimental gate

## 3. Values

- [x] 3.1 Defaults at startup, overridden by what is stored; drop stored values that are no longer declared (D4)
- [x] 3.2 Validation: type, range, length, allowed values, all-or-nothing (D3)
- [x] 3.3 Store only what changed, one settings key per parameter (D4)
- [x] 3.4 Publish the set as twin state on every connect and after every accepted change, on both transports (D2)

## 4. The operation

- [x] 4.1 `c8y_ParameterUpdate` through the JSON operation path, answered by id (D6)
- [x] 4.2 A refusal names the parameter and what was wrong, and changes nothing
- [x] 4.3 The application's hook may refuse, and the client rolls back

## 5. The schema

- [x] 5.1 Generate the JSON Schema from the declaration (D5)
- [x] 5.2 A shell command to print it, behind `TEDGE_PARAMETERS_SCHEMA`

## 6. Use it

- [x] 6.1 The Modbus application declares its pump parameters and applies them
- [x] 6.2 The application's existing Kconfig defaults become the declared defaults, so nothing is configured twice

## 7. Tests

- [x] 7.1 Unit tests: validation of each type, including the edges of a range and an unknown name
- [x] 7.2 Unit tests: the twin JSON and the generated schema for a mixed set
- [x] 7.3 Kconfig cases: parameters selectable; the API absent when it is off

## 8. Hardware verification

- [x] 8.1 C6: the set appears in the cloud with its current values
- [x] 8.2 A change is applied, reported, and visible in the twin
- [x] 8.3 A value outside its range fails, naming the parameter, and changes nothing
- [x] 8.4 A change the application refuses is rolled back
- [x] 8.5 Values survive a reboot; a parameter removed by a firmware update disappears
- [x] 8.6 Core MQTT: the same set, reported the same way
- [x] 8.7 Footprint row

## 9. Wrap-up

- [x] 9.1 README: declaring parameters, what the cloud sees, what a bad value does, and how to get the schema
- [x] 9.2 Profiles, design.md results, SCOPE roadmap P7; archive
