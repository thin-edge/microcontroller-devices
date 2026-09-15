# lib/frontend-template — skeleton for a new protocol frontend

Copy this directory to `lib/<protocol>` (e.g. `lib/snmp`, `lib/modbus`,
`lib/canbus`) as the starting point for a new industrial-protocol frontend, then:

1. Rename `template_frontend.{c,h}` → `<protocol>_frontend.{c,h}` and the entry
   point `template_frontend_start()` → `<protocol>_frontend_start()`.
2. Rename the module in `zephyr/module.yml` and the Kconfig symbol/menu.
3. Implement the wire protocol, mapping the **shared data model** onto it:
   - measurements → `data_source_count/descriptor/sample` (from `lib/common`)
   - writable control points → `app_control_setpoint/set_setpoint/running/set_running`
   - identity → `app_identity_device_id/firmware_name/firmware_version/build_timestamp`
4. Add your protocol stack under `third_party/` if needed (vendored), and wire it
   in `CMakeLists.txt`.
5. Create `apps/<protocol>-source/` (or `-server`) composing `lib/common` +
   `lib/<protocol>`, with its own `prj.conf`, `Kconfig`, `VERSION`, `boards/`,
   and `CONFIG_APP_FIRMWARE_NAME` default (e.g. `zephyr-snmp-source`).

See `lib/common/README.md` for the full protocol-frontend contract. This
template is not composed by any application, so it is never compiled as-is.
