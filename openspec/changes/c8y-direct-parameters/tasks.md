## 1. Settle the mechanism

- [ ] 1.1 Confirm the tenant has Parameter Update (Digital Twin Manager and the device-parameter microservice), and capture what a `c8y_ParameterUpdate` operation actually carries, before writing device code
- [ ] 1.2 Register one hand-written schema in the tenant and change a value against a mock device, so the round trip is understood end to end

## 2. The declaration

- [ ] 2.1 The public API: `struct tedge_parameter`, the `TEDGE_PARAM_*` macros, `tedge_declare_parameters()` and the change hook (D1)
- [ ] 2.2 Remove the configuration-file API from the header, with a line saying what replaced it (D7)
- [ ] 2.3 Kconfig: `TEDGE_PARAMETERS`, `TEDGE_PARAMETERS_MAX`, `TEDGE_PARAMETERS_SCHEMA`; drop `TEDGE_CONFIG` and its experimental gate

## 3. Values

- [ ] 3.1 Defaults at startup, overridden by what is stored; drop stored values that are no longer declared (D4)
- [ ] 3.2 Validation: type, range, length, allowed values, all-or-nothing (D3)
- [ ] 3.3 Store only what changed, one settings key per parameter (D4)
- [ ] 3.4 Publish the set as twin state on every connect and after every accepted change, on both transports (D2)

## 4. The operation

- [ ] 4.1 `c8y_ParameterUpdate` through the JSON operation path, answered by id (D6)
- [ ] 4.2 A refusal names the parameter and what was wrong, and changes nothing
- [ ] 4.3 The application's hook may refuse, and the client rolls back

## 5. The schema

- [ ] 5.1 Generate the JSON Schema from the declaration (D5)
- [ ] 5.2 A shell command to print it, behind `TEDGE_PARAMETERS_SCHEMA`

## 6. Use it

- [ ] 6.1 The Modbus application declares its pump parameters and applies them
- [ ] 6.2 The application's existing Kconfig defaults become the declared defaults, so nothing is configured twice

## 7. Tests

- [ ] 7.1 Unit tests: validation of each type, including the edges of a range and an unknown name
- [ ] 7.2 Unit tests: the twin JSON and the generated schema for a mixed set
- [ ] 7.3 Kconfig cases: parameters selectable; the API absent when it is off

## 8. Hardware verification

- [ ] 8.1 C6: the set appears in the cloud with its current values
- [ ] 8.2 A change is applied, reported, and visible in the twin
- [ ] 8.3 A value outside its range fails, naming the parameter, and changes nothing
- [ ] 8.4 A change the application refuses is rolled back
- [ ] 8.5 Values survive a reboot; a parameter removed by a firmware update disappears
- [ ] 8.6 Core MQTT: the same set, reported the same way
- [ ] 8.7 Footprint row

## 9. Wrap-up

- [ ] 9.1 README: declaring parameters, what the cloud sees, what a bad value does, and how to get the schema
- [ ] 9.2 Profiles, design.md results, SCOPE roadmap P7; archive
