## Why

A device that cannot be updated in the field has a short life. Cumulocity's
firmware operation is the standard way to do it, and the pieces are already
on the boards: MCUboot with a second slot, the layout this repository's
applications are built with, and a client that holds a token for the tenant.

`c8y-direct-spikes` proved the whole path on an ESP32-C6 (Spike B, unknowns
U5 and U6): a real Cumulocity binary downloaded over HTTPS at 79 KB/s, an
MCUboot swap, a test boot, confirmation only after the new image reached
Cumulocity, and a rollback that reported itself — the rollback fired for real
when a Zephyr HTTP-client bug corrupted an image, and the safety net worked.
`c8y-direct-core` and `c8y-direct-remote-access` then built the module around
it. This change moves firmware update out of `apps/c8y-spike` and into
`tedge-zephyr` (roadmap step P3).

## What Changes

- **`CONFIG_TEDGE_FIRMWARE_UPDATE` becomes real.** On `515,<device>,<name>,
  <version>,<url>` the client reports `501`, streams the image into the
  secondary slot, requests a test boot and resets.
- **Confirmation is earned, not assumed.** The new image confirms itself only
  after it has connected to Cumulocity *and* the application's
  `firmware_confirm_check` hook has passed. Otherwise MCUboot reverts it on
  the next reset, and the old image reports `502` with the reason, so a bad
  image cannot brick a fleet.
- **Downloads that work in the field:** up to three redirects (a GitHub
  release asset sends a 913-byte `Location`), the JWT sent only to the
  tenant's own domain, the image written from the parser's `on_body`
  callback (Zephyr's HTTP client corrupts chunked bodies otherwise, spike
  problem P5), and a longer TLS connect timeout for hosts outside the tenant.
- **State the operator can see:** `115,<name>,<version>` on every connect so
  the inventory shows what is running, and the expected downtime named in the
  operation while the swap happens (P7: the device is dark for ~40 s on the
  C6, ~19 s on the S3).
- **Boards without MCUboot cannot select it:** the Kconfig dependency already
  exists; this change makes it load-bearing.

## Non-goals

- Telemetry, logs, configuration and certificate renewal (P4–P7).
- Delta or compressed updates, and updating anything other than the
  application image (the bootloader, the provisioner, the Wi-Fi blobs).
- Removing `apps/c8y-spike`: it goes once this change is verified, since
  firmware update is the last feature it still demonstrates alone.
- Changing MCUboot's swap strategy (P7 suggests measuring swap-using-move;
  that is a separate experiment).

## Resource constraints

From Spike B on the ESP32-C6, on top of the client:

| Item | Cost |
|---|---|
| Text | ~55 KB (HTTP client, DFU, the flow) |
| TLS heap | MQTT + one HTTPS download: **90.9 KB peak** with 16 KB records |
| Static RAM | a download thread (8 KB stack in the spike), a ~1 KB URL buffer, the HTTP receive buffer |
| Flash | a second slot the size of the image, already in the layout |
| Time | download 11.4 s for 898 KB, swap 40 s (C6) / 19 s (S3), operation SUCCESSFUL in 69 s |

Boards: the ESP32-C6 and ESP32-S3 (which needs its TLS heap in PSRAM to
afford a second session). The WROOM cannot run this feature beside MQTT.

## Capabilities

### New Capabilities

- `tedge-firmware-update`: the operation, the download, the test boot and
  the confirmation rules.

### Modified Capabilities

- `device-management-features`: the baseline says a feature must be
  selectable and advertise itself; firmware update adds the requirement that
  an image is confirmed only after it proves itself, and that the running
  version is reported.
- `tedge-client-module`: `firmware_confirm_check` becomes a requirement with
  a scenario, next to the restart and remote-access hooks.

## Impact

- `tedge-zephyr/src/`: new `tedge_firmware.c` (the flow) and
  `tedge_http_download.c` (redirects, `on_body` writing, the JWT rule), and a
  dispatcher entry in `tedge_c8y.c`.
- Zephyr facilities: `http_client`, `flash_img`/`dfu`, `mcuboot`, the
  existing sockets and credentials.
- Applications: the C6 and S3 Modbus profiles gain the feature; the image
  must be built with sysbuild and MCUboot, as these boards already are.
- Carries spike problems P5 (report the HTTP-client bug upstream) and P7
  (the downtime during a swap).
