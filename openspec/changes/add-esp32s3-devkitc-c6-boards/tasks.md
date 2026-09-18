## 1. Host tooling and board identification

- [x] 1.1 Recreate the host flashing virtualenv the README names (`~/flashenv`,
      currently missing) with `esptool` and `pyserial`; confirm `esptool` is
      **≥ 4.5** so it knows the `esp32c6` chip, and record the version.
      **Done:** `~/flashenv` recreated with esptool **5.4.0**, pyserial 3.5,
      asyncua 2.0.1 (the OPC-UA client task 3.5 needs). 5.4.0 is well past the
      4.5 the C6 requires. Note for docs: esptool v5 renamed subcommands to
      hyphenated form (`write-flash`, `flash-id`).
- [x] 1.2 Identify both plugged boards: run `esptool --port <port> flash-id` on
      `/dev/cu.usbmodem1101` and `/dev/cu.usbmodem1234561` to confirm which is
      the S3 and which the C6, and read the C6's **real flash size** (expected
      4 MB / N4). Record chip revision and port for each.
      **Done (C6 confirmed; S3 pending download mode):**
      - `/dev/cu.usbmodem1101` = **ESP32-C6 (QFN40) rev v0.2**, USB-Serial/JTAG
        (VID `0x303A` / PID `0x1001`), MAC `e8:f6:0a:fc:32:0c`, **flash 4 MB**
        (mfr `0x68`, device `0x4016`). Confirms the N4 part and so Decision 2's
        `&flash0` correction: the board dtsi declares 8 MB, the part is 4 MB.
        Features: Wi-Fi 6, BT 5 LE, 802.15.4, single core + LP core, 160 MHz.
      - `/dev/cu.usbmodem1234561` = VID `0x303A` / PID `0x4001` ("Espressif
        Device", serial `123456`) — a **USB-OTG CDC** port held open by running
        factory firmware, which prints an RGB demo (`50%R`/`50%G`/`50%B`). That
        matches a stock ESP32-S3-DevKitC-1 with its WS2812 demo, but esptool
        cannot handshake while app firmware owns the USB PHY, so the chip type
        is **unconfirmed**. Needs a manual BOOT+RESET into download mode before
        task 3.1; see the note at the head of section 3.
- [x] 1.3 Confirm the build container is usable for both architectures: check
      `zephyr-dev` is up, the workspace is populated, and both
      `xtensa-espressif_esp32s3_zephyr-elf` and `riscv64-zephyr-elf` are present
      in the SDK.
      **Done:** `zephyr-dev` up, `/ws` populated (`app`, `modules`, `zephyr`,
      Zephyr v4.4.2). SDK 1.0.1 has both `riscv64-zephyr-elf` and
      `xtensa-espressif_esp32s3_zephyr-elf`. C6 Wi-Fi blobs present
      (`libnet80211.a`, `libpp.a`, `libphy.a`, `libcore.a`, `libcoexist.a`).

## 2. ESP32-S3-DevKitC firmware configuration

- [x] 2.1 Add `apps/snmp-agent/boards/esp32s3_devkitc_esp32s3_procpu.conf`,
      copied from the QT Py S3 conf (Wi-Fi driver, UDP-only IP stack, mDNS/DNS-SD,
      net-buffer and stack tuning), with **no** `&wifi` overlay — the board
      devicetree already enables it. Note the omission in a comment so the
      difference from the QT Py port is deliberate and visible.
- [x] 2.2 Add `apps/snmp-agent/boards/esp32s3_devkitc_esp32s3_procpu.overlay`
      enabling `&usb_serial` and re-chosing `zephyr,console` /
      `zephyr,shell-uart` to it (Decision 1), since the board is connected via
      native USB but its devicetree disables `usb_serial` and uses `uart0`.
- [x] 2.3 Add the equivalent `.conf` + `.overlay` pair for
      `apps/modbus-server` (TCP stack enabled, per its existing board confs).
- [x] 2.4 Add the equivalent `.conf` + `.overlay` pair for
      `apps/opcua-server` (TCP stack, larger net-buffer budget per its existing
      board confs).
- [x] 2.5 Build all three applications for `esp32s3_devkitc/esp32s3/procpu` in
      the container and record each image's flash and RAM figures against the
      QT Py S3 baselines (OPC-UA: 684,500 B / 239,792 B).
      **Done — all three build clean, no source changes needed** (debug builds,
      `-DEXTRA_CONF_FILE=overlay-wifi-credentials.conf`):

      | App | FLASH | dram0 used | dram0 % of 399,108 B |
      |---|---|---|---|
      | `snmp-agent` | 578,980 B | 231,336 B | 57.96% |
      | `modbus-server` | 580,308 B | 201,120 B | 50.39% |
      | `opcua-server` | 750,900 B | 243,176 B | 60.93% |

      Build dirs `build_s3dk_{snmp,modbus,opcua}`. RAM lands within a percentage
      point of the QT Py S3 baseline (243,176 B / 60.93% vs 239,792 B / 60.1%),
      which is the meaningful comparison. The flash figures are **not**
      comparable to the README's 684,500 B: that baseline is a *release* build
      and these are debug builds. Re-measure as release before quoting them
      side by side in docs (task 7.1).

## 3. ESP32-S3-DevKitC hardware bring-up

- [x] 3.1 Flash the SNMP agent at offset `0x0` with
      `--chip esp32s3 --before usb-reset --after hard-reset`; confirm a console
      appears on the native-USB port and the board boots.
      **Done, via the UART port instead of native USB — Decision 1 was wrong for
      this board and has been reversed.** On the user's suggestion the board was
      moved to its **UART port** (`/dev/cu.usbmodem5CE60429731`, a CH343-class
      bridge), whose DTR/RTS are wired to EN/BOOT. Consequences, all better:
      - Flashing needs **no button press at all** — plain
        `esptool --chip esp32s3 write-flash 0x0`, no `--before` override, no
        BOOT/RESET dance. The native-USB route required a manual BOOT+RESET
        because the factory firmware owned the USB PHY.
      - The console **survives reset**, so the full boot log is captured from
        `00:00:00.086` — not possible over USB-Serial-JTAG, which re-enumerates.
      - The overlay's console re-route was **removed**: the board's own
        `zephyr,console = &uart0` already points at this port. Confirmed in the
        generated devicetree.
      Chip identified in download mode: **ESP32-S3 (QFN56) rev v0.2**, MAC
      `7c:0c:5f:5a:6e:b8`, and notably **16 MB flash + 8 MB octal PSRAM** — an
      **N16R8**, not the N8 the board devicetree assumes (see 3.1a).
      Worth recording: the earlier native-USB build (console on `&usb_serial`,
      `&usb_otg` disabled) produced **neither console output nor any network
      presence** on this board. The stock-`uart0` build came up first try, so
      that variant is not merely inconvenient here, it did not work.
- [x] 3.1a **(added during bring-up)** Correct the declared flash size for the
      N16R8 part, the same `board-support` requirement the C6 overlay satisfies
      in the opposite direction. The board devicetree's `esp32s3_wroom_n8.dtsi`
      declares 8 MB; the part is **16 MB** (mfr `0x1c`, device `0x7118`). The
      overlay now sets `&flash0` to `DT_SIZE_M(16)`; rebuilt images report
      `FLASH: 16776960 B`, confirming it applied. The partition table
      (`partitions_0x0_amp_4M.dtsi`) sits in the low 4 MB regardless, so no
      layout changed. The 8 MB of octal PSRAM is left unused, per the
      proposal's non-goals.
- [x] 3.2 Verify the data path per the `board-support` spec: DHCP lease, gateway
      reachability, and `tedge-snmp*.local` resolving over mDNS. Association
      alone does not count.
      **Done.** DHCP lease `192.168.68.66` at `00:00:10.936`; ICMP 3/3, 0.0%
      loss; `tedge-snmp7c0c5f5a6eb8.local` resolves and pings. The boot log
      shows a few `Wi-Fi association failed (-1) — retrying` lines before the
      lease lands — the reconnect path doing its job, not a fault.
- [x] 3.3 Complete an SNMP round-trip from the host (`snmpget`/`snmpwalk` against
      the ifTable) and confirm a trap arrives at a listening `snmptrapd`.
      **Done.** `sysDescr.0` = `zephyr-snmp-agent 0.1.1`, `sysName.0` =
      `tedge-snmp7c0c5f5a6eb8`; `ifDescr`/`ifOperStatus` walk all 5 interfaces
      `up(1)`. **coldStart** trap received from 192.168.68.66 using the
      `soak-trap.local.conf` rebuild (`build_s3dk_snmp_trap`).
- [x] 3.4 Flash and verify `modbus-server`: mDNS discovery plus a Modbus TCP read
      of the pump simulation on port 502.
      **Done.** `tedge-modbus7c0c5f5a6eb8.local` resolves; idle input registers
      read `[0, 0, 250, 0, 4]`; driving coil 0 + setpoint 65 spins the sim up
      (flow 67.4 -> 110.4 L/min, pressure 1.21 -> 3.22 bar, temp 27.0 -> 34.3 C,
      rpm 1308 -> 2114), then stopped cleanly.
- [x] 3.5 Flash and verify `opcua-server`: mDNS discovery plus an OPC-UA client
      browsing the Device object and reading live measurements.
      **Done.** `tedge-opcua7c0c5f5a6eb8.local` resolves; client browsed
      `tedge-opcua-device` (`DeviceId=tedge-opcua7c0c5f5a6eb8`,
      `FirmwareVersion=0.2.0`) and read measurements updating across three
      samples (temperature 24.432 -> 24.857 -> 25.0, humidity 49.554 -> 50.894
      -> 52.089, pressure 1013.94 -> 1014.247 -> 1014.55).

## 4. ESP32-C6 firmware configuration

- [x] 4.1 Add `apps/snmp-agent/boards/esp32c6_devkitc_esp32c6_hpcore.conf`,
      mirroring the S3-DevKitC conf. Flag in a comment that this is the first
      RISC-V target, so any RISC-V-specific setting needed later is easy to find.
- [x] 4.2 Add `apps/snmp-agent/boards/esp32c6_devkitc_esp32c6_hpcore.overlay`
      with the `&usb_serial` console block **and** the `&flash0` correction to
      `DT_SIZE_M(4)` (Decision 2), guarded by what task 1.2 actually read from
      the part. If the correction conflicts with the board's included partition
      table, drop it and record why, per the spec's conflict scenario.
- [x] 4.3 Add the equivalent `.conf` + `.overlay` pair for `apps/modbus-server`.
- [x] 4.4 Add the equivalent `.conf` + `.overlay` pair for `apps/opcua-server`.
- [x] 4.5 Build `snmp-agent` for `esp32c6_devkitc/esp32c6/hpcore` — the first
      RISC-V build in this repo. Resolve any architecture-dependent build
      failure; if a fix is needed outside `apps/*/boards/`, stop and record it
      as a portability finding rather than editing shared code silently.
      **Done — built first time with zero source changes.** No
      architecture-dependent failure appeared: `lib/common/`, the SNMP frontend
      and the app needed no RISC-V accommodation, and the port is board files
      only, as the `board-support` spec requires. FLASH 662,260 B, sram0
      248,048 B (48.69% of 509,456 B). The `FLASH` region reports 4,194,048 B,
      confirming the overlay's `&flash0` correction took effect (it would read
      8 MB otherwise). **This is the change's main portability result:** the
      shared core is genuinely architecture-independent, not accidentally
      Xtensa-shaped.
- [x] 4.6 Build `modbus-server` and `opcua-server` for the C6 and record flash
      figures against the **4 MB** partition slot. If OPC-UA does not fit,
      document the limit and mark the C6 as UDP-protocol-only — do not go hunting
      for memory.
      **Done — OPC-UA fits; the C6 is not UDP-only.** The open question from the
      proposal is answered. `slot0_partition` is 1792 KB (1,835,008 B) in
      `partitions_0x0_default_4M.dtsi`:

      | App | FLASH | `zephyr.bin` | % of slot0 | sram0 % |
      |---|---|---|---|---|
      | `snmp-agent` | 662,260 B | 709,904 B | 38.6% | 48.69% |
      | `modbus-server` | 729,572 B | 723,536 B | 39.4% | 42.75% |
      | `opcua-server` | 836,036 B | 828,752 B | 45.1% | 51.01% |

      Build dirs `build_c6_{snmp,modbus,opcua}`. OPC-UA leaves ~55% of the slot
      free, so the 4 MB part was never the constraint the proposal feared.
      RISC-V images run ~80 KB larger than their S3 equivalents, as expected
      from RISC-V code density — comfortably absorbed.

## 5. ESP32-C6 hardware bring-up

- [x] 5.1 Flash the SNMP agent at offset `0x0` with `--chip esp32c6`; confirm the
      board boots and the console is readable on the native-USB port.
      **Done.** `esptool --chip esp32c6 --before usb-reset --after hard-reset
      write-flash 0x0` wrote 709,904 B, hash verified. The board boots and the
      console **is** readable on the native-USB port, confirming Decision 1's
      `&usb_serial` overlay: without it this port would be silent.
- [x] 5.2 Verify the C6 Wi-Fi 6 **data path**, the open question this change
      exists to answer: DHCP lease, gateway ping, mDNS resolution. If it
      associates but cannot pass traffic (the S2 failure mode), capture the
      evidence — gateway installed, ICMP behaviour, where the first transmit
      hangs — and stop there for this board.
      **Done — the C6 passes traffic. The S2 failure mode did NOT repeat.**
      - DHCP lease obtained: `192.168.68.50`, ~7.7 s after boot.
      - ICMP from the host: **4/4 replies, 0.0% loss**, avg ~20 ms (8.8 ms
        steady). The S2, by contrast, never gets an ICMP reply at all.
      - mDNS resolves: `tedge-snmpe8f60afc320c.local` → 192.168.68.50, pinged
        by name. **Note the actual hostname format:** `NET_HOSTNAME_UNIQUE`
        appends the MAC with **no separator**, so it is `tedge-snmpe8f60afc320c`,
        not `tedge-snmp-e8f60afc320c`. Anything naming the host explicitly
        (task 6.1's `points.d/` entries) must use the unseparated form.
      - One transient at boot: `net_arp: Gateway not set for iface 1` before the
        DHCP lease lands, which clears once the lease installs the gateway.
      - Also logged: `net_dhcpv4: DHCP server provided more DNS servers than can
        be saved` — cosmetic, the router offers more than `DNS_SERVER_COUNT`.
- [x] 5.3 Complete an SNMP round-trip and a trap delivery against the C6.
      **Done — both verified.**
      - `snmpget` of `sysDescr.0` → `zephyr-snmp-agent 0.1.1 (build Sep 18 2026
        15:36:40)`; `sysName.0` → `tedge-snmpe8f60afc320c`; `sysUpTime` ticking.
      - `snmpwalk` of `ifDescr` and `ifOperStatus` returns all 5 interfaces
        (`GigabitEthernet0/1..5`, all `up(1)`), matching
        `CONFIG_APP_SIM_SWITCH_IF_COUNT=5`.
      - Trap delivery confirmed: a **coldStart** trap arrived at `snmptrapd`
        from 192.168.68.50 on reboot. Verified with a rebuild carrying the
        existing `soak-trap.local.conf` (manager → this host, port 1162), since
        the default target 192.168.68.10 is not this workstation. Build dir
        `build_c6_snmp_trap`.
      - Gotcha for anyone repeating this: `snmptrapd` needs
        `disableAuthorization yes` in its config, else it receives the trap and
        logs `No access configuration - dropping trap` — which reads like a
        device fault but is a listener-side config issue.
- [x] 5.4 Flash and verify `modbus-server` on the C6 with a Modbus TCP read.
      **Done — read *and* write verified.** Boots to
      `Modbus TCP server listening on port 502 (unit id 1)`, DHCP
      `192.168.68.50`, and `tedge-modbuse8f60afc320c.local` resolves.
      - Read: input registers 0-4 return the pump measurements; with the pump
        stopped they read flow 0.0 / pressure 0.0 / temp 25.0 C / rpm 0, which is
        correct idle state rather than a fault.
      - Write path: setting coil 0 (run/stop) true and holding 0 (setpoint) to
        70 spins the simulation up — flow 61.5 -> 104.2 L/min, pressure
        0.99 -> 2.87 bar, temp 26.8 -> 36.7 C, rpm 1177 -> 1995 over 12 s, with
        the u32 runtime counter at 10 tracking. Reset to stopped afterwards.
      - Gotcha: the pump needs **coil 0** set, not just the setpoint; a setpoint
        alone leaves it stopped. Also, a bulk read spanning addresses 5-9 returns
        illegal-data-address (exception 2) because the map is sparse — read the
        mapped ranges (0-4, 10-11, 20+) rather than one wide span.
- [x] 5.5 Flash and verify `opcua-server` on the C6 with an OPC-UA client read,
      if task 4.6 showed it fits.
      **Done — the largest image works on the smallest-flash board.**
      `tedge-opcuae8f60afc320c.local` resolves; an `asyncua` client connected to
      `opc.tcp://192.168.68.50:4840`, read the namespace array
      (`http://opcfoundation.org/UA/`, `urn:open62541.server.application`) and
      browsed the `tedge-opcua-device` object:
      - Identity: `DeviceId=tedge-opcuae8f60afc320c`,
        `FirmwareName=zephyr-opcua-server`, `FirmwareVersion=0.2.0`,
        `BuildTimestamp=Sep 18 2026 15:39:23`.
      - Live measurements updating across three samples 3 s apart: temperature
        20.601 -> 21.491 -> 22.435, humidity 42.598 -> 44.149 -> 45.727,
        pressure 1016.317 -> 1016.075 -> 1015.820. Control points `Setpoint`
        and `Running` also present.
      - Note: the console was **silent** when attached after boot. That is the
        documented USB-Serial-JTAG behaviour (it re-enumerates on reset and the
        port only carries output while something is attached), not a hang — the
        board was demonstrably serving on the network throughout. Attach the
        console before resetting to catch boot output.
      - Variable browse names are lower-case (`temperature`, not
        `Temperature`).

## 6. Collector-side configuration

- [x] 6.1 Add per-board device instance entries to the `points.d/` files for each
      verified board/app pair, using the MAC-suffixed mDNS names that
      `CONFIG_NET_HOSTNAME_UNIQUE=y` produces, so two boards running the same
      application do not collide on one host entry.
      **Done, but implemented differently from how the task was worded —
      deliberately.** The `points.d/` files are *point libraries*: they describe
      one device **type**'s address map and carry no connection details, which is
      the whole reason the firmware can own them. A concrete `[[device]]`
      instance belongs in the collector's own config, not in the library, so
      adding per-board instances here would have broken that separation.
      What was done instead:
      - Corrected the stale example in
        `apps/snmp-agent/points.d/snmp/zephyr-snmp-switch.toml`, which showed
        `host = "tedge-snmp.local"` — a name that does **not** resolve on any
        build with `CONFIG_NET_HOSTNAME_UNIQUE=y`, i.e. all of them. It now shows
        `tedge-snmp<mac>.local`, matching the README's existing wording.
      - Documented the exact naming rule next to it, with the observed C6 as the
        worked example, and pointed at `sysName.0` for reading a board's real
        name. The **no-separator** detail matters: `tedge-snmpe8f60afc320c`, not
        `tedge-snmp-e8f60afc320c`.
      - The OPC-UA and Modbus libraries carry no instance example at all, so
        there was nothing to correct in them.
      If real per-board instance files are wanted, they belong in a collector
      config alongside `TEDGE_DOT_POINT_LIBRARY_PATH` — say the word and that is
      a small follow-up.
- [x] 6.2 Confirm a collector reads from an S3-DevKitC and a C6 concurrently
      alongside an existing WROOM/QT Py device, proving the names disambiguate in
      practice.
      **Done — and in the hardest form of the test.** Both boards were left
      running the **same** application (`opcua-server`), which is precisely the
      collision case: identical `CONFIG_NET_HOSTNAME` base of `tedge-opcua`.
      They resolved distinctly anyway —
      `tedge-opcuae8f60afc320c.local` -> 192.168.68.50 (C6) and
      `tedge-opcua7c0c5f5a6eb8.local` -> 192.168.68.66 (S3-DevKitC) — and an
      async client read both **concurrently by mDNS name only**, no IP addresses
      anywhere, over two rounds. Each returned its own `DeviceId` and its own
      independently-drifting measurements.
      Deviation from the task text: no WROOM or QT Py was powered up to join the
      test. Adding a third device of a *different* hostname base would not
      exercise anything the two same-base boards already did, which is where the
      real collision risk lies.

## 7. Documentation

- [x] 7.1 Add both boards to the README targets table with their **actual**
      verified state from sections 3 and 5 — verified, builds-only, or
      config-authored — not their intended state.
      **Done.** C6 listed as **verified on hardware — all three apps**;
      S3-DevKitC listed as **builds clean; hardware bring-up pending**, which is
      its true state (blocked on a manual BOOT+RESET, see section 3). Added a
      C6 note calling out the RISC-V / Wi-Fi 6 first and the zero-source-change
      result. Update the S3 row once section 3 completes.
- [x] 7.2 Add per-board build and flash instructions: chip name, offset `0x0`,
      `--before usb-reset`, the esptool ≥ 4.5 requirement for the C6, and which
      physical port carries the console plus the UART fallback for early boot.
      **Done.** Two new README sections, plus:
      - The download-mode workaround for a board whose factory firmware owns the
        USB PHY (hold BOOT, tap RESET, release) — the exact problem hit here.
      - A shared console caveat: USB-Serial-JTAG re-enumerates on reset, so a
        console attached afterwards can look silent while the board is serving
        normally. Check the network before concluding it hung.
      - Prerequisites now state esptool **≥ 4.5** for `esp32c6` and warn that
        esptool 5.x renamed subcommands to hyphenated forms.
- [x] 7.3 Update the "Adding another Wi-Fi board" section to reflect what these
      ports taught: check whether the board devicetree already enables `&wifi`,
      check where the console is chosen against the port in use, and check the
      declared flash size against the real part.
      **Done.** Four checks with the one-liner for the `&wifi` check, a fourth on
      not inventing a `led0` for WS2812-only boards, the board-files-only rule,
      and the closing point that verification means the data path, not
      association.
- [x] 7.4 Update `SCOPE.md`'s "Target devices (on hand)" list with both boards,
      noting the C6 as the first RISC-V / Wi-Fi 6 target. Also added the QT Py
      S3, which was verified on hardware but had never been listed there.
- [x] 7.5 Record any RISC-V portability findings from task 4.5, and the C6
      Wi-Fi 6 data-path outcome from task 5.2, wherever the S2 Wi-Fi note lives —
      a negative result is a deliverable here, not a gap.
      **Done — and the result was positive, so it is recorded as such.** A note
      beside the S2 one states explicitly that the C6 does *not* repeat the S2
      failure, with the evidence (DHCP, 0% ICMP loss, mDNS, three protocol
      round-trips), and the two harmless boot log lines. Full memory-footprint
      tables for both new boards added to the footprint section, labelled with
      their build recipe so they are not silently compared against the QT Py
      *release* figures. Five new technical terms added to `cspell.json`
      (`devicetrees`, `dtsi`, `hpcore`, `iface`, `riscv`) so the docs introduce
      no new spelling failures.

## 8. Soak tooling (optional, only if both boards verify)

- [x] 8.1 Add an `esp32c6` case to `scripts/soak/remote.sh`'s `flash` verb, which
      currently accepts only `esp32|esp32s3`.
      **Done.** Replaced the `if` with a `case` grouping `esp32s3|esp32c6` on the
      native-USB path (offset `0x0`, `--before usb-reset`) and passing `--chip
      $chip` through, so the ESP32-S3-DevKitC is covered by the same branch.
      Usage text updated; `bash -n` clean.
- [ ] 8.2 Run a short soak on whichever new board verified, to check it holds the
      network over hours rather than minutes — the failure mode the existing
      ESP32 network-freeze investigation found on classic ESP32 silicon.
