## Why

Every device has settings that someone eventually wants to change from the
cloud: a reporting interval, a threshold, a mode, a target address. The
client can already carry an operation and store things in settings; what it
cannot do is let an operator see those settings, change one, and know it
took.

The API declared for this (`tedge_register_config_type()`) was drawn for
**configuration files**: a snapshot reader, a chunked writer, an upload and
a download. That shape came from thin-edge.io on Linux, where a
configuration *is* a file. On a microcontroller it is the wrong shape:

- there is no filesystem, so both sides are inventing one;
- a snapshot is an opaque blob an operator cannot read or edit safely;
- it needs the HTTP transfer path in both directions for what is usually
  fewer than ten values;
- nothing validates anything: a bad file is discovered by a device that
  stops working.

Cumulocity's **Parameter Update** feature fits an MCU far better, and
thin-edge.io itself is moving that way ([tedge-parameter-plugin]): a device
declares a **set of typed parameters** with a JSON Schema, their current
values live in the device twin where an operator can see them, and a change
arrives as one `c8y_ParameterUpdate` operation carrying only the values.
This client already publishes twin fragments and, since `c8y-operation-ids`,
receives operations as JSON with their id — so parameters are mostly
plumbing it already has.

This is phase 3, roadmap step P7, on the same ESP32-C6 and ESP32-S3 boards,
with the Modbus application as the example.

[tedge-parameter-plugin]: https://github.com/thin-edge/tedge-parameter-plugin

## What Changes

- **`CONFIG_TEDGE_PARAMETERS` replaces `CONFIG_TEDGE_CONFIG`.** The
  configuration-file API (`tedge_register_config_type()`,
  `tedge_config_reader_t`, `tedge_config_writer_t`) is **removed from the
  public header** — it was never implemented, and shipping a shape the
  client will not honour is worse than removing it.
- **An application declares a parameter set:** a name and a table of
  parameters, each with a type (bool, integer, string, enum), a default, and
  its limits (range, length, allowed values).
- **Values live in three places that agree:** the client's settings subtree
  (so they survive a reboot), the device twin (so an operator can see them),
  and the application (through a change hook).
- **`c8y_ParameterUpdate` is handled:** the client validates every value
  against what was declared, stores and applies the ones that pass, calls
  the application, republishes the twin, and completes the operation by id —
  or fails it naming the parameter that was wrong, changing nothing.
- **The client can print its own JSON Schema** (a shell command and a log
  line), because the schema has to be registered in the tenant by a human
  and hand-writing it from a C table is a transcription error waiting to
  happen.
- **Nothing is sampled, uploaded or downloaded**: no HTTP, no filesystem, no
  new TLS session.

## Non-goals

- **Configuration files** (`c8y_Configuration`, snapshots and updates). If a
  device ever needs to ship a blob, that is a separate change and it needs
  the upload path, not this one.
- Nested or repeated structures. A parameter is one value with one type;
  an application that wants a table of them can encode it, and say so.
- Registering the schema in the tenant. That is the tenant owner's job, as
  the Smart Functions are; the client only produces the schema.
- Changing parameters from the device side as a general API. The
  application owns its own settings; this is about the cloud's view of them.

## Resource constraints

| Item | Cost |
|---|---|
| Text | validation, the twin JSON, the schema printer (behind its own option) |
| RAM | the declared table is the application's `const` data; the client keeps only current values, bounded by `TEDGE_PARAMETERS_MAX` and the value sizes declared |
| Flash | one settings key per parameter, written only when a value changes |
| Network | one twin message per connect and per change; one operation per change |

## Capabilities

### New Capabilities

- `tedge-parameters`: what a device can declare, what an operator can see
  and change, what happens to a change that does not fit, and what survives
  a reboot.

### Modified Capabilities

- `device-management-features`: configuration management becomes parameter
  management, and stops being an unimplemented feature.
- `tedge-client-module`: registering configuration types becomes declaring
  parameters, with the `-ENOTSUP` rule when the feature is not built in.

## Impact

- `tedge-zephyr/include/tedge/tedge.h`: the configuration-file API is
  replaced by the parameter API. Nothing in the repository uses the old one
  (it never had a definition), so nothing breaks.
- `tedge-zephyr/src/`: a new `tedge_parameters.c` (the table, validation,
  storage, twin) and the `c8y_ParameterUpdate` path in `tedge_c8y.c`.
- The Modbus application declares its pump parameters (interval, setpoint
  limits, mode), so the repository shows the hook in use.
- The tenant: the parameter set's schema has to be registered with the
  Digital Twin Manager, and the Parameter Update feature has to be available
  — which the first task checks before anything else is written.
