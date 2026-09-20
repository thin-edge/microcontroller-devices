## Why

When a device in the field misbehaves, the question is always the same: what
happened? Today the only answers are a serial console someone has to stand
next to, or a remote-access tunnel — which needs the device to still be
healthy enough to open one. Neither helps with the device that rebooted an
hour ago, and neither leaves a record.

This is phase 3, roadmap step P5, targeting the same boards as the rest of
the client (ESP32-C6 and ESP32-S3 primary; the Modbus application is the
example host). It gives the device three ways to explain itself without
anyone travelling to it:

- **its own log**, kept in a RAM ring and uploadable on request, so the last
  minutes before a problem survive the question being asked;
- **a crash dump**, written to the flash partition the layouts already
  reserve and offered after the reboot — the only record that outlives the
  fault that erased the RAM log;
- **a shell command**, allow-listed, for the question the logs do not answer.

Two defects fall out of the same work and are fixed here, because both are
already promised in the baseline spec:

- `tedge_register_log_type()` and `tedge_register_config_type()` are
  **declared but never defined**. An application that calls them fails to
  link, where the spec requires "not supported" and the README promises
  `-ENOTSUP`.
- An operation the image cannot run — `c8y_LogfileRequest` today — is
  **silently left pending** in Cumulocity instead of being failed with a
  reason, which the "Operation for an absent feature" scenario forbids.

## What Changes

- **`CONFIG_TEDGE_SHELL_COMMAND` becomes real:** `c8y_Command` runs an
  allow-listed Zephyr shell command on its own thread and returns what it
  printed. The allow-list is **empty by default**: until the integrator
  lists what may run, every command is refused with a reason. A cloud that
  can run arbitrary shell commands on a field device is a bigger risk than
  the feature is worth by default.
- **`CONFIG_TEDGE_LOG_UPLOAD` becomes real:** the client answers
  `c8y_LogfileRequest`, advertises the log types it has, applies the
  request's filters, and uploads the result as a Cumulocity event binary.
- **The client keeps its own log** in a bounded RAM ring (a Zephyr log
  backend), offered as a log type without the application doing anything.
  This is what makes the feature useful on a device whose application has no
  log of its own.
- **Crash dumps:** with `CONFIG_DEBUG_COREDUMP` the client writes a dump to
  the `coredump_partition` the board layouts already reserve, and offers the
  most recent one as a log type after the reboot, with the tooling to decode
  it documented.
- **The application can add log types** through `tedge_register_log_type()`,
  which acquires a definition — and an `-ENOTSUP` one when the feature is
  off, so an application builds either way. The same goes for
  `tedge_register_config_type()` (still P7 otherwise).
- **HTTP gains upload**, and `CONFIG_TEDGE_HTTP` finally owns the HTTP
  sources it was invented for (they are gated on firmware update today).
- **An operation the image cannot run is failed, not dropped**, with a reason
  naming what is missing.

## Non-goals

- Streaming a log continuously to the cloud. A log is fetched when someone
  asks; a device that narrates itself over MQTT costs bandwidth for data
  nobody reads.
- A filesystem. Log types are produced by callbacks, not read from files;
  the module keeps its "no filesystem required" property.
- Configuration management (P7), although it shares the upload path being
  built here.
- Decoding the crash dump on the device. The dump is uploaded as it was
  written; the host tooling reads it.

## Resource constraints

| Item | Cost |
|---|---|
| Text | the shell capture, the log ring backend, the coredump reader, and HTTP upload (a POST with a multipart body) |
| RAM | `TEDGE_LOG_RING_BYTES` (default 2048) for the client's log, a capture buffer while a command runs, and one upload buffer; the upload thread's stack is shared with the download's |
| Flash | the 4 KB `coredump_partition` already present in every board layout |
| Network | one HTTPS POST per log request; nothing periodic |
| Time | a command is bounded by a timeout; an upload runs off the client thread so the connection keeps working |

## Capabilities

### New Capabilities

- `tedge-diagnostics`: what a device can be asked for when it misbehaves —
  its log, its last crash, and an allow-listed command — and what it must
  refuse.

### Modified Capabilities

- `device-management-features`: the shell command and log upload stop being
  unimplemented; an operation for an absent feature is failed rather than
  ignored.
- `tedge-client-module`: registering a log type becomes a requirement with
  scenarios, including what happens when the feature is not built in.

## Impact

- `tedge-zephyr/src/`: new `tedge_shell_cmd.c`, `tedge_log_upload.c`,
  `tedge_log_ring.c` (the log backend), `tedge_coredump.c`, and
  `tedge_http_upload.c`; `tedge_c8y.c` gains templates 522 and 118 and the
  refusal path; `CMakeLists.txt` re-gates the HTTP sources on
  `CONFIG_TEDGE_HTTP`.
- `tedge-zephyr/include/tedge/tedge.h`: definitions for the registration
  calls, and `-ENOTSUP` stubs when the features are off.
- Kconfig: `TEDGE_SHELL_COMMAND_ALLOW_LIST`, `TEDGE_SHELL_COMMAND_TIMEOUT_S`,
  `TEDGE_SHELL_COMMAND_OUTPUT_BYTES`, `TEDGE_LOG_RING_BYTES`,
  `TEDGE_LOG_UPLOAD_MAX_BYTES`, `TEDGE_COREDUMP`; the two
  `TEDGE_FEATURE_AVAILABLE_*` gates flip to `default y`.
- The Modbus application gains a log type of its own, so the repository
  shows an application using the hook.
