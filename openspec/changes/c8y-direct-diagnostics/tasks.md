## 1. Groundwork

- [x] 1.1 Confirm with curl against the tenant how a log binary is uploaded and what URL comes back (event binaries vs `/inventory/binaries`, multipart vs plain body), and record the answer in design.md (D4, open question)
- [x] 1.2 `TEDGE_HTTP`: give it the Zephyr selects it needs and move `tedge_http_download.c` + `tedge_url.c` (and `TEDGE_FIRMWARE_MAX_REDIRECTS`) onto it in CMakeLists/Kconfig (D6)
- [x] 1.3 `tedge_http_upload.c`: POST with a body callback, Bearer JWT for the tenant only, redirects as the download does (D6)

## 2. The operation paths

- [x] 2.1 Fail an operation the image cannot run, with a reason naming the missing feature, instead of leaving it pending (D8)
- [x] 2.2 Definitions for `tedge_register_log_type()` and `tedge_register_config_type()`, plus `-ENOTSUP` stubs in the header when the feature is off, so an application links either way
- [x] 2.3 Publish `118` (supported log types) on every connect, beside `114` (D7)

## 3. The shell command

- [x] 3.1 `tedge_shell_allow_list.c`: pure prefix matching, metacharacter refusal, empty list refuses everything (D1)
- [x] 3.2 `tedge_shell_cmd.c`: run through the dummy shell backend on its own thread, capture output, one at a time, timeout (D1)
- [x] 3.3 Dispatch `511`/`c8y_Command`: `501` first, then `503` with the output or `502` with the reason (D7)
- [x] 3.4 Kconfig: `TEDGE_SHELL_COMMAND_ALLOW_LIST`, `_TIMEOUT_S`, `_OUTPUT_BYTES`; select `SHELL_BACKEND_DUMMY`; flip the feature-available gate

## 4. Logs

- [x] 4.1 `tedge_log_ring.c`: a Zephyr log backend into a bounded ring in the module heap, oldest dropped (D3)
- [x] 4.2 `tedge_log_upload.c`: registry of log types, the measure-then-stream upload clamped to the measured length, truncation notice at the cap (D2, D4)
- [x] 4.3 Dispatch `522`/`c8y_LogfileRequest` with its filters, uploading off the client thread and answering `503,…,<url>` (D7)
- [x] 4.4 Kconfig: `TEDGE_LOG_RING_BYTES`, `TEDGE_LOG_UPLOAD_MAX_BYTES`; flip the feature-available gate

## 5. Crash dumps

- [x] 5.1 `tedge_coredump.c`: detect a stored dump at boot, offer it as a log type, emit the `#CD:` text format, erase only after the cloud has it (D5)
- [x] 5.2 Kconfig `TEDGE_COREDUMP`: select `DEBUG_COREDUMP` with the flash-partition backend; document the partition requirement

## 6. Use it

- [x] 6.1 The Modbus application registers a log type of its own, so the repository shows the hook in use
- [x] 6.2 A test-only way to force a fault (shell command behind an app Kconfig), for verifying the dump path

## 7. Tests

- [x] 7.1 Unit tests: the allow-list (prefix match, arguments, metacharacters, empty list)
- [x] 7.2 Unit tests: the log ring (fits, wraps, oldest dropped) and the filters a reader is given
- [x] 7.3 Kconfig cases: the two features selectable without EXPERIMENTAL; log upload selects HTTP; shell command still needs SHELL; update `unimplemented-feature-locked` so only configuration management stays locked

## 8. Hardware verification

- [x] 8.1 C6: the log types appear in Cumulocity and the client's own log uploads and is readable
- [x] 8.2 The request's filters are honoured (a search text and a line limit)
- [x] 8.3 An allow-listed command returns its output; a command that is not allow-listed, and one with a metacharacter, are refused with a reason
- [x] 8.4 With no allow-list configured, every command is refused
- [x] 8.5 A forced fault produces a dump that uploads and decodes with Zephyr's parser
- [x] 8.6 An operation for a feature that is not built in fails with a reason instead of hanging
- [x] 8.7 The connection keeps working during an upload and a slow command
- [x] 8.8 Footprint row with diagnostics enabled

## 9. Wrap-up

- [x] 9.1 README: the log-type API, the allow-list and why it is empty, what the ring can hold, and how to decode a dump
- [x] 9.2 Profiles: diagnostics in `full.conf` (drop its experimental caveat where it no longer applies); design.md results; SCOPE roadmap P5
