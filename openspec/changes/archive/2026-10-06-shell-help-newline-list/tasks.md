## 1. Firmware: keep line breaks in c8y_Command results

- [x] 1.1 In `tedge-zephyr/src/tedge_smartrest.c`, move the body of `tedge_sr_quote()` into a static helper with a `keep_newlines` flag; add `tedge_sr_quote_lines()` that keeps `\n` and drops `\r`; declare it in `tedge_internal.h`
- [x] 1.2 In `tedge-zephyr/src/tedge_c8y.c`, add line-keeping variants of `op_succeeded`/`op_failed` (sharing one static helper) and use them in the `CONFIG_TEDGE_SHELL_COMMAND` event loop; update the "newlines become spaces" comment
- [x] 1.3 Check that no other caller of `tedge_sr_quote()` changes behaviour and that no new buffers were added

## 2. Unit tests (native_sim)

- [x] 2.1 Add `tedge_smartrest` tests for `tedge_sr_quote_lines()`: `\n` kept, `\r\n` becomes `\n`, quotes still doubled, truncation still closes the quote
- [x] 2.2 Keep the existing `tedge_sr_quote()` newline-to-space test passing unchanged
- [x] 2.3 Run the `tedge-zephyr/tests/unit` suite and the Kconfig tests

## 3. Hardware check against the dev tenant

- [x] 3.1 Build modbus-server tedge-full with the shell-diagnostics extra for the S3-DevKitC and compare flash/RAM with the previous build (expect only a few bytes of flash)
- [x] 3.2 Flash it, send `help` as a c8y_Command, and confirm in Cumulocity that the operation is SUCCESSFUL and the result shows one command per line
- [x] 3.3 Send a multi-line allowed command (e.g. `net iface`) and a refused one; confirm both results are well formed and the device stays connected
- [x] 3.4 If Cumulocity rejects or splits the multi-line value, switch to the fallback in design.md (a visible separator in `tedge_shell_help_text()`) and record why — not needed: eu-latest accepted it (2026-10-06, S3-DevKitC `tedge-modbusd405927b33d0`, app +112 B flash, RAM unchanged)

## 4. Docs

- [x] 4.1 Update the shell diagnostics note in `DEVICES.md` to say `help` lists the commands one per line
