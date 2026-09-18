## Context

The repo builds three Zephyr applications (`apps/opcua-server`,
`apps/modbus-server`, `apps/snmp-agent`) from a shared `lib/common/` core plus
one protocol frontend each. A board port today is purely additive configuration:
a `apps/<app>/boards/<fully-qualified-board>.conf` (Wi-Fi driver, IP stack, mDNS,
hostname, net-buffer and stack budget) and, where the board devicetree needs
adjusting, a sibling `.overlay`. Zephyr auto-merges both when the build targets
the application directory. No application or shared-core source has ever had to
change for a board.

Four hardware targets exist. Only two are verified: `esp32_devkitc` (WROOM-32)
and `adafruit_qt_py_esp32s3`. The S2 Feather builds but cannot pass Wi-Fi traffic
under Zephyr 4.4.2, and the Pico W config has never been built. All verified
silicon is Xtensa.

This change adds two targets, both physically on hand and plugged in:

| Board | Zephyr target | SoC | Flash | Notes |
|---|---|---|---|---|
| ESP32-S3-DevKitC-1 | `esp32s3_devkitc/esp32s3/procpu` | Xtensa LX7, dual-core | 8 MB (N8 dtsi) | Espressif reference board |
| QIQIAZI ESP32-C6 (WROOM-1-N4) | `esp32c6_devkitc/esp32c6/hpcore` | **RISC-V**, single-core | **4 MB part**, 8 MB in dtsi | First RISC-V + Wi-Fi 6 target |

**What the environment already provides** (verified in the `zephyr-dev`
container against the pinned Zephyr v4.4.2):

- Both boards ship upstream: `boards/espressif/esp32s3_devkitc`,
  `boards/espressif/esp32c6_devkitc`. No `west.yml` change.
- Both board devicetrees already contain `&wifi { status = "okay"; }`, so unlike
  the QT Py S3 and the S2 Feather **neither port needs a Wi-Fi overlay**.
- Zephyr SDK 1.0.1 carries `xtensa-espressif_esp32s3_zephyr-elf` and
  `riscv64-zephyr-elf`.
- `hal_espressif` C6 Wi-Fi blobs are fetched: `libnet80211.a`, `libpp.a`,
  `libphy.a`, `libcore.a`, `libcoexist.a`.
- `WIFI_ESP32` requires `!SMP`. The C6 is single-core; the S3 builds as `procpu`
  only. Both qualify.

**What the environment does not provide.** Both boards enumerate over their
**native USB-Serial-JTAG** port (`USB JTAG_serial debug unit` and
`Espressif Device`); no CP210x/CH34x UART bridge is present on the host. But the
`esp32s3_devkitc_procpu.dts` sets `&usb_serial { status = "disabled"; }` and
routes `zephyr,console` to `uart0`, and `esp32c6_devkitc_hpcore.dts` likewise
chooses `uart0`. Out of the box, neither board would produce a console on the
port in use. Separately, the host virtualenv the README names (`~/flashenv`) no
longer exists.

## Goals / Non-Goals

**Goals:**

- Six working builds: two boards × three applications, each producing an image
  that fits the board's real flash and RAM.
- A console reachable on the port each board is actually plugged into, so
  bring-up is observable.
- Prove the shared core and all three frontends are architecture-independent by
  running them on RISC-V — or find precisely where they are not.
- Keep the port additive: `boards/` files only, no `lib/` or `apps/*/src`
  changes. Any change forced outside `boards/` is a finding to report, not to
  absorb.
- Leave the porting contract written down, so the next board is mechanical.

**Non-Goals:**

- PSRAM bring-up on either board, and the S3-DevKitC's octal-PSRAM variants.
- The C6's IEEE 802.15.4 / Thread radio and Bluetooth on either board.
- Any fix for the ESP32-S2 Wi-Fi data path.
- CI integration for the new targets.
- Deduplicating the per-board `.conf` files (see Decision 5).

## Decisions

> **Outcome note (written after bring-up).** Decision 1 below was **reversed for
> the ESP32-S3-DevKitC** and held only for the C6. The S3-DevKitC has a second,
> UART socket whose bridge drives DTR/RTS, which makes it strictly better: it
> flashes with no button press and its console survives reset, so boot output is
> not lost. The native-USB variant also simply *did not come up* on that board —
> no console and no network — whereas the stock `uart0` console worked first
> try. Decision 4's "verify in increasing order of risk" is what made this cheap
> to find. The C6 board on hand exposes only one USB socket, so it keeps the
> overlay as described. Decision 2 was confirmed and gained a mirror case: the
> S3-DevKitC is an **N16R8** (16 MB flash, 8 MB octal PSRAM) against a
> devicetree assuming N8, so its overlay corrects `&flash0` *upward*.

### Decision 1: Console over native USB-Serial-JTAG, via a board overlay

Each board gets an overlay enabling `&usb_serial` and re-chosing the console:

```dts
&usb_serial { status = "okay"; };

/ {
	chosen {
		zephyr,console = &usb_serial;
		zephyr,shell-uart = &usb_serial;
	};
};
```

This mirrors the already-verified QT Py ESP32-S3, whose *board* devicetree does
exactly this (`zephyr,console = &usb_serial`) — so the pattern is known-good on
S3 silicon in this Zephyr, and the C6 SoC dtsi exposes an equivalent
`espressif,esp32-usb-serial` node.

*Alternative considered — use the stock `uart0` console over the UART bridge.*
Rejected as the default: on the S3-DevKitC that is a physically different USB
port needing a second cable and a CP210x driver, and neither board is currently
enumerated that way. It stays documented as the fallback, and it is the better
option for anyone debugging early boot, because a USB-Serial-JTAG console
re-enumerates on reset and so drops the first moments of boot output — the same
known limitation the QT Py S3 already has.

### Decision 2: Correct the C6 flash size in devicetree, not Kconfig

`esp32c6_devkitc_hpcore.dts` includes `esp32c6_wroom_n8.dtsi`, which declares
`&flash0 { reg = <0x0 DT_SIZE_M(8)>; }`. The module on hand is an **N4**, so the
declaration overstates the part by 2×. There is no `ESPTOOLPY_FLASHSIZE` Kconfig
symbol in this `hal_espressif` — the flash size is taken from devicetree — so the
fix belongs in the board overlay:

```dts
&flash0 { reg = <0x0 DT_SIZE_M(4)>; };
```

**This is correctness hygiene, not a blocker.** The partition table the board
pulls in is `partitions_0x0_default_4M.dtsi`, i.e. every partition already lives
below 4 MB, and esptool auto-detects the real flash size when writing. So the
build is expected to work even without the overlay; the overlay stops anything
that reasons from `flash0`'s size (a future storage partition, a bootloader flash
-size check) from believing in 4 MB that does not exist. If the overlay turns out
to conflict with the included partition table, dropping it is an acceptable
outcome to record.

### Decision 3: No status LED on either port

`lib/common/status_led.c` drives a plain GPIO via the `led0` alias. Neither new
board defines `led0`: both carry a **WS2812 addressable RGB LED**, which needs
`CONFIG_LED_STRIP` and an RMT/SPI driver and does not fit the GPIO API the
indicator is written against. The QIQIAZI C6 is a clone whose LED pin is not
documented upstream anyway.

Both ports therefore rely on the behaviour `wifi-connectivity` already specifies
for boards with no LED: the indicator is a no-op and the firmware runs normally.
Connectivity state is read from the console log instead.

*Alternative considered — add WS2812 support to the status indicator.* Rejected:
it costs flash and RAM on the board where flash is tightest, changes shared-core
code this port is meant not to touch, and is a worthwhile change in its own right
rather than a rider on a board port.

### Decision 4: Verify in increasing order of risk

Bring-up order is **S3-DevKitC before C6**, and within each board **SNMP →
Modbus → OPC-UA**.

The S3-DevKitC is near-identical silicon to the verified QT Py S3, so it isolates
"new board" problems from "new architecture" problems: if the S3 port works and
the C6 does not, the cause is RISC-V or the C6 radio, not the porting method.
Within a board, the SNMP agent is the smallest image (UDP-only, no TCP) and the
OPC-UA server the largest (684,500 B flash / 239,792 B RAM on the QT Py S3), so
this order gets a working Wi-Fi + mDNS data path proven on the cheap image before
flash pressure is added as a variable.

### Decision 5: Copy the per-board `.conf` files rather than factoring them out

The two new boards bring the count of near-identical ESP32 `.conf` files to
6 apps×boards more, each repeating the same Wi-Fi/IP/mDNS block and the same
measured net-buffer and stack tuning (`NET_BUF_RX_COUNT=32`,
`SYSTEM_WORKQUEUE_STACK_SIZE=2048`, `ZVFS_POLL_MAX=6`,
`NET_SOCKETS_SERVICE_STACK_SIZE=2400`) with its explanatory comments.

The duplication is real and is accepted here. Zephyr matches `boards/<board>.conf`
by fully-qualified board name with no include mechanism, so sharing would mean
either an `EXTRA_CONF_FILE` on every build — which breaks the documented
one-line `west build -b <board> apps/<app>` — or moving the defaults into each
app's `Kconfig` behind a `SOC_FAMILY_ESPRESSIF_ESP32` condition. The latter is a
genuine improvement and is the right follow-up, but folding a config refactor
into a board port would make a failed build ambiguous between the two. Copying
keeps this change's failures attributable.

The tuning values carry over unchanged because they were measured from
*workload* behaviour (mDNS socket count, log/workqueue stack peaks under the
protocol threads), not from SoC specifics.

## Risks / Trade-offs

- **OPC-UA may not fit the C6's 4 MB flash.** → The image is 684,500 B on the
  S3, so raw size is not the concern; the partition slot is. Verified early by
  building before flashing, and the bring-up order means SNMP and Modbus are
  already proven on the C6 by the time OPC-UA is attempted. If it does not fit,
  record the C6 as a UDP-protocol target and stop — the proposal's non-goals rule
  out hunting for memory.
- **C6 Wi-Fi may be immature under Zephyr 4.4.2, as it is on the S2.** → The
  blobs and Kconfig are present, but presence is not function; this is the same
  shape of risk that made the S2 unusable. Mitigated by testing the *data path*
  (DHCP lease, gateway ping, an actual protocol round-trip) rather than
  accepting association as success — exactly the trap the S2 investigation
  documented. A broken C6 data path is a reportable outcome, not a failure of
  this change.
- **The QIQIAZI board is a clone, not an Espressif C6-DevKitC-1.** → Pin
  assignments (LED, buttons, strapping) may differ from the upstream board
  definition. The port uses only the SoC-level Wi-Fi and USB-Serial-JTAG
  peripherals, which are on-die and clone-independent; nothing depends on board
  pinout. This is why Decision 3's "no LED" is the safe call rather than a
  guess at a GPIO.
- **A USB-Serial-JTAG console hides early boot.** → Accepted, matching the QT Py
  S3. The UART-console fallback is documented for anyone who needs boot output.
- **Two boards running the same app collide on mDNS hostname.**
  `CONFIG_NET_HOSTNAME_UNIQUE=y` appends a MAC-derived suffix, so they resolve
  distinctly, but `points.d/` instance files that name a host must get per-board
  entries.
- **Flashing procedure differs per chip.** → Both boards use offset `0x0` (both
  include `partitions_0x0_*`), unlike the WROOM's `0x1000`, and native USB needs
  `--before usb-reset`. The C6 additionally needs **esptool ≥ 4.5**, and the
  README's `~/flashenv` venv is gone and must be recreated. Documented per board.

## Migration Plan

Additive only — no existing board, app, or spec behaviour changes, so there is
nothing to migrate and rollback is deleting the new files. The README's targets
table gains two rows whose status must state what was actually verified rather
than what was intended.

## Spec corrections after bring-up

Two `board-support` requirements were written before any hardware ran and were
corrected at archive time, so the durable spec records what was verified rather
than what was assumed:

- **Console.** The original requirement prescribed re-chosing the console onto
  `&usb_serial` whenever a board's devicetree pointed elsewhere. Bring-up
  inverted that: where a board has a UART socket, that socket is *better* — its
  bridge drives DTR/RTS, so esptool resets the board itself (no BOOT/RESET
  press) and the console survives a reset, giving boot output from the first
  line. On the S3-DevKitC the native-USB variant additionally produced neither
  console nor network. The requirement now prefers the DTR/RTS-capable port and
  reserves the `&usb_serial` overlay for boards without one, with a third
  scenario recording that a silent late-attached console is not evidence of a
  hang.
- **Flash size.** The original covered only a devicetree *overstating* the part
  (C6: 8 MB declared, 4 MB fitted). The S3-DevKitC supplied the mirror case
  (8 MB declared, 16 MB fitted), so the requirement now spans both directions.

## Open Questions

- Does the C6's Wi-Fi 6 radio associate and pass IPv4 traffic under Zephyr
  4.4.2, or does it repeat the S2's associate-but-no-data-path failure? This is
  the question the change exists to answer and cannot be settled from config.
- Does the OPC-UA image fit the C6's 4 MB flash partition slot?
- Is the QIQIAZI clone's flash really 4 MB (N4), and does it match the
  `esp32c6_devkitc` board definition closely enough to boot unmodified?
  Confirmed by reading the chip with `esptool flash-id` during bring-up.
- Should the shared ESP32 Kconfig defaults be factored out (Decision 5) as a
  follow-up change once these ports are verified?
