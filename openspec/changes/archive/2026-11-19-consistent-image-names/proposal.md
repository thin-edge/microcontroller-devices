## Why

The release images showcase one device agent on three operating systems, but
their names only sometimes say which one:

- Zephyr: never (`modbus-server-tedge-full-esp32c6-devkitc`).
- ESP-IDF: only in the app's name (`tedge-agent-idf-tedge-onboard-esp32c6-devkitc`).
- NuttX: only in the variant (`tedge-agent-nuttx-esp32c6-devkitc`).

Someone comparing operating systems cannot tell from a release file, a
firmware entry in Cumulocity or the firmware name a device reports which
operating system an image runs, nor which images are the same app.

## What Changes

- **BREAKING** Every image is named `<app>-<os>-<set>-<board>`:
  - `<app>`: `modbus-server`, `opcua-server`, `snmp-agent` or `tedge-agent`.
    The ESP-IDF agent is `tedge-agent` too; its project stays in
    `apps/tedge-agent-idf/`.
  - `<os>`: `zephyr`, `idf` (ESP-IDF, which includes FreeRTOS) or `nuttx`.
  - `<set>`: `standalone` (the protocol server alone), `full` (every
    thin-edge.io feature) or `ota` (connection, telemetry, restart and
    firmware update, plus what else fits).
  - `<board>`: the device id, as before.

  For example `tedge-agent-zephyr-full-esp32c6-devkitc`,
  `tedge-agent-idf-full-esp32c6-devkitc` and
  `tedge-agent-nuttx-full-esp32c6-devkitc`. The name is the release files'
  stem, the firmware name the image reports, and its firmware entry in
  Cumulocity. The longest is 47 characters, the most a device stores.
- The manifest's variants are renamed `<os>-<set>`: `zephyr-standalone`,
  `zephyr-full`, `zephyr-ota`, `idf-full` and `nuttx-full`. The manifest
  rejects a variant whose name does not match its framework and set, and
  two builds with the same firmware name.
- The README's supported-devices table and the release notes show the
  variants by operating system.
- A device running an image with an old name moves to the new one with a
  single update from the new firmware entry. The device does not compare
  firmware names when it installs, so nothing on the device changes.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `firmware-release`: the variants, the asset stem and the firmware name.

## Impact

- `release/devices.yml`, `scripts/release/` (matrix, packaging, notes,
  tests), `release/size-baseline.json` (keyed by firmware name), the
  README, DEVICES.md (a note on the old names), and the release workflow's
  job names.
- Cumulocity: a release uploads under new firmware entries. The old entries
  stay; each device needs one update from its new entry.
- No firmware code changes. Local builds, outside the release, keep their own
  names (`zephyr-<app>`, and `tedge-agent-idf-<chip>` on ESP-IDF).

## Non-goals

- Renaming `apps/tedge-agent-idf/` or the Zephyr apps' directories.
- Changing device types, the version scheme or the board ids.
