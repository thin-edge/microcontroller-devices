## Context

Spike B (archived in `2026-09-19-c8y-direct-spikes`) ran the whole firmware
path on an ESP32-C6: a Cumulocity binary over HTTPS into slot1, an MCUboot
swap, a test boot, confirmation after reaching the cloud, and a revert. Its
code is `apps/c8y-spike/src/spike_ota.c`, driven from a shell.

What the spike established, and this design takes as given:
- **MCUboot swap-scratch takes ~40 s on the C6** (~19 s on the S3) for a
  ~900 KB image, and the device is offline for it. `prov`, `bootreq` and
  `storage` come through untouched.
- **An unconfirmed image is reverted** on the next reset, and the old image
  finds the pending marker and reports the rollback. This fired for real.
- **Zephyr's HTTP client mis-reports chunked bodies** in its response
  callback: one receive buffer can hold several segments, and the client
  keeps the first segment's start with the last segment's length. Cumulocity
  serves binaries chunked, so the image must be written from the parser's own
  `on_body` callback (P5).
- **Binary URLs use the tenant-ID host** (`t<id>.<domain>`), not the tenant's
  host name, so "is this Cumulocity?" is a parent-domain rule, and the token
  must never travel anywhere else.
- **Redirects are not followed** by Zephyr's client, and a GitHub release
  asset sends a 913-byte `Location`.
- MQTT plus one HTTPS download peaks at **90.9 KB of TLS heap**.
- A CPU reset hangs MCUboot on the C6; the module's platform reset already
  handles this (`c8y-direct-core`, D8).

## Goals / Non-Goals

**Goals:** the feature in the module behind `CONFIG_TEDGE_FIRMWARE_UPDATE`;
an image that cannot confirm itself is always reverted and always reported;
downloads that work against Cumulocity and a plain HTTPS host; the running
version visible in the inventory.

**Non-Goals:** delta updates, updating the bootloader or the provisioner,
changing the swap strategy (P7), and keeping the spike app afterwards.

## Decisions

### D1: The download is its own file, usable by later features

`tedge_http_download.c` takes a URL, an optional bearer token and a sink
callback, and handles redirects (3 hops), the `on_body` writing rule and the
token-domain rule. `tedge_firmware.c` passes a sink that writes into the
MCUboot slot through `flash_img`. Log upload and configuration (P5/P7 on the
roadmap) will reuse the same downloader with different sinks.

*Alternative: fold it into the firmware feature.* Rejected: two later
features need exactly this, and it is the part with the subtle rules.

### D2: The flow, and where the marker lives

```
515 -> 501 (EXECUTING, with the expected downtime in the text)
    -> download into the secondary slot   (~11 s for 900 KB)
    -> persist "pending: <name>,<version>" in settings
    -> request a test boot, platform reset
    -> MCUboot swaps                      (~40 s C6 / ~19 s S3: offline)
    -> new image boots "not confirmed", connects, hooks pass
    -> confirm, 115,<name>,<version>, 503, clear the marker
```

If the new image never connects, or the application's
`firmware_confirm_check` refuses, the image stays unconfirmed and MCUboot
reverts it on the next reset. The old image then finds the marker while
running *confirmed*, which is how it knows a revert happened, and reports
`502,c8y_Firmware,"…"` with the reason.

The marker is a settings key (`tedge/firmware`), so it survives the swap and
a power cut. It is the only state the flow keeps.

### D3: The download runs on its own thread

Like the remote-access bridge, the download blocks; the client thread must
keep servicing MQTT so the operation's progress reaches Cumulocity and the
session does not time out. `TEDGE_FIRMWARE_STACK_SIZE` defaults to **8192**,
the value the remote-access bridge needed for a TLS handshake on its own
thread.

### D4: The token goes to the tenant only

The client sends its bearer token when the URL's host ends in the tenant's
parent domain (`tedge-dev05.preprod.c8y.io` → `.preprod.c8y.io`), which
covers the tenant-ID host Cumulocity uses for binaries. Any other host, and
any host reached through a redirect, gets no token. A redirect that leaves
the tenant's domain drops the token for the rest of the transfer.

### D5: Confirmation needs the cloud and the application

`TEDGE_FIRMWARE_CONFIRM_AFTER_CONNECT` (default y) confirms once the client
reaches Cumulocity. The application's `firmware_confirm_check` hook runs
first: it can look at whatever it needs (its protocol server, a sensor, a
peer) and refuse. A refusal leaves the image unconfirmed, so the next reset
reverts it. With the option off, only the application's own call confirms an
image, for devices that want a longer probation.

### D6: What the operator sees

- `501` carries the expected downtime, so a 40-second silence is not
  mistaken for a failure.
- `115,<name>,<version>` goes out on every connect, so the inventory always
  shows what is running, not what was last installed.
- A revert reports `502` naming the version that failed.

### D7: Kconfig

`TEDGE_FIRMWARE_UPDATE` already depends on `BOOTLOADER_MCUBOOT` and selects
`TEDGE_HTTP`. It adds `TEDGE_FIRMWARE_STACK_SIZE`,
`TEDGE_FIRMWARE_CONFIRM_AFTER_CONNECT` and `TEDGE_FIRMWARE_MAX_REDIRECTS`
(3), and selects `IMG_MANAGER`, `MCUBOOT_IMG_MANAGER`, `STREAM_FLASH` and
`IMG_ERASE_PROGRESSIVELY` (the spike needed progressive erase to keep the
download from stalling).

### D10: An unconfirmed image resets itself

An image that cannot reach the cloud never confirms, and **nothing else
would reset the device**: the network is up, so the application's
connectivity watchdog sees nothing wrong, and the client simply retries with
a growing back-off. Seen on hardware: a bad image ran for 8 minutes, happily
retrying, while MCUboot waited for a reset that never came.

So a test-booted image arms `TEDGE_FIRMWARE_CONFIRM_TIMEOUT_S` (default
900 s) at start-up and resets itself when it expires, which lets the
bootloader roll it back. Confirmation cancels it.

**The limit of this:** the deadline lives in the *new* image, so it only
protects against images that carry it. An image without it (an older build,
or one that crashes before the client starts) still needs an external reset
— a hardware watchdog, or the application's own. This is worth saying aloud
in the README: MCUboot's revert is the safety net, and the deadline is what
pulls the trigger.

### D8: The same version is refused

A `515` whose name and version match the running image SHALL be failed at
once with a reason, before anything is downloaded (decided with the tenant
owner, 2026-09-20). Re-installing the running image costs a 900 KB download
and ~40 s of downtime for no change, and an operator who wants that can
install a different version or erase the device. The check uses the version
MCUboot reports for the running image, not the marker.

### D9: Progress on a free-form topic, QoS 0

Progress is published to `te/device/<id>///progress/firmware` at QoS 0
(decided with the tenant owner, 2026-09-20): it is a stream of hints, not
state, so losing one costs nothing and it must not sit in a queue behind
device-management traffic.

```json
{"name":"zephyr-modbus-server","version":"0.3.0","phase":"downloading",
 "percent":45,"bytes":405504,"total":898378}
```

Cumulocity serves binaries **chunked**, so there is no `Content-Length` and
no percentage to report: progress then carries `bytes` only, every 128 KB.
When a server does send a length (a plain file host), `percent` is included
and the step is `TEDGE_FIRMWARE_PROGRESS_PERCENT` (default 10).

Phases: `downloading` (never more than one message a second), then `installing` (the test boot is
requested; the device is about to go offline for the swap), then `done` or
`failed` with a `reason`. A tenant maps the topic with a Smart Function, the
same way as the twin data.

Core MQTT has no free-form topics, so a Core MQTT build publishes no
progress; the operation's `501`/`503`/`502` still tell the story. The option
`TEDGE_FIRMWARE_PROGRESS` (default y with the MQTT Service) turns it off for
devices on a metered link.

## Risks / Trade-offs

- [The device is offline for the swap (P7)] → the downtime is in the `501`
  text and the README; measuring swap-using-move stays a separate task.
- [A corrupt image reaches the slot] → MCUboot's signature check rejects it,
  the old image boots and reports the rollback. Proven by the chunked-body
  bug.
- [An image that connects but is broken in a way the hook cannot see] → the
  hook is the application's to write; the README says what belongs in it.
- [A download holds a TLS session for ~11 s] → the profiles size the heap for
  MQTT plus one download; remote access and a download at once need a third
  session, which the C6 cannot afford with 16 KB records.
- [The token could leak to a redirect target] → the domain rule, tested.

## Migration Plan

- `apps/c8y-spike` is deleted once this change is verified: firmware update
  is the last feature it demonstrates alone. The archived spike design keeps
  its measurements.
- Devices already running a spike image are test devices; they get the
  module's firmware through the same operation.

## Open Questions

- None outstanding; the two questions above were answered on 2026-09-20.

## Results

### First hardware run (C6, Modbus + client, 2026-09-20)

| Check | Result |
|---|---|
| Install 0.3.0 from Cumulocity | SUCCESSFUL: 887 KB downloaded in 28 s, swap, confirmed 14 s after the reboot, ~105 s end to end |
| The inventory afterwards | `c8y_Firmware` shows the running version, read from MCUboot's header, not the requested one |
| Install the running version | FAILED, "zephyr-modbus-server 0.6.0 is already running"; nothing downloaded, no reboot |
| Install an older version (0.6.0 → 0.4.0) | SUCCESSFUL: downgrades are allowed, only the *same* version is refused |
| Progress | `te/device/<id>///progress/firmware` at QoS 0: `downloading` every 128 KB, then `installing`, then `done` |

### Rollback (tasks 4.3, 2.5)

| Check | Result |
|---|---|
| An image that cannot reach the cloud (a tenant host that does not resolve) | it downloads and boots, never confirms, and after 180 s resets itself; MCUboot restores the previous image |
| What the operator sees | the restored image reports `502`: "1.2.0-deadline-test was rolled back; the device is running 1.1.0", and the inventory shows the restored version |
| Progress | a final `{"phase":"failed","reason":"rolled back"}` message |
| Without the deadline (an image built before it) | the bad image ran for 8 minutes retrying; the rollback only happened when something else reset the device. This is why D10 exists |

**Three findings, all fixed:**

1. **A refusal must publish `501` first.** Cumulocity's `502` fails the
   oldest *executing* operation, so an operation refused straight out of
   PENDING never left that state. The client now reports `501` for every
   firmware operation it receives, then `502` with the reason if it refuses.
   This also cleared a queue of operations that had piled up.
2. **No percentage for a Cumulocity download.** The binary is chunked, so
   `Content-Length` is absent; progress reports `bytes` every 128 KB instead,
   and `percent` only when a server provides a length.
3. **The version to compare is MCUboot's,** not the application's
   `APP_VERSION_STRING`: an image built from the same source with a different
   signed version must still be installable. The client reads the running
   version from the image header, and logs it at every connect.
