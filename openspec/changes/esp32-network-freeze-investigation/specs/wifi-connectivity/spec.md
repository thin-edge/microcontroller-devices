## ADDED Requirements

### Requirement: Sustained connectivity

A Wi-Fi device SHALL stay reachable on its protocol endpoint over extended
operation. Every interruption SHALL end in automatic recovery within a bounded
time and never leave the device unreachable until someone power-cycles it. This
covers an access-point restart, loss of radio contact, a network-level outage,
and the firmware itself stalling. The recovery bound is the relevant configured
recovery timeout plus the normal bring-up time:
- for a network-level outage, the last-resort reboot timeout;
- for a firmware stall, the liveness watchdog timeout.

An outage longer than that bound is an **unrecovered outage**.

#### Scenario: Long soak without unrecovered outages

- **WHEN** a supported ESP32 board runs a protocol app (OPC-UA, Modbus or SNMP)
  for 24 hours while its protocol endpoint is polled every few seconds
- **THEN** there are zero unrecovered outages
- **AND** every outage that does occur, and every watchdog reset, is recorded
  with its cause

#### Scenario: Access point restart

- **WHEN** the access point restarts or the device briefly loses radio contact
- **THEN** the device reconnects and serves again within the recovery bound

#### Scenario: Firmware stall

- **WHEN** the firmware stalls so that the connectivity watchdog itself no
  longer runs
- **THEN** the liveness watchdog resets the device, and it serves again within
  the recovery bound

## MODIFIED Requirements

### Requirement: Last-resort self-reboot

The firmware SHALL reboot itself if it stays offline for a configurable,
prolonged period despite repeated reconnect attempts, so it recovers on its own.
The timeout SHALL be configurable in Kconfig, and the behaviour SHALL be
possible to disable.

This is the **network-level** recovery. The periodic connectivity watchdog
evaluates it, so it only works while that watchdog is still running. If the
firmware itself stalls, the liveness watchdog (capability `firmware-liveness`)
recovers the device instead.

#### Scenario: Prolonged outage self-recovers

- **WHEN** the device has been offline continuously for longer than the
  configured last-resort timeout despite reconnect attempts
- **THEN** it reboots itself and runs the normal bring-up again

#### Scenario: Reboot disabled

- **WHEN** the last-resort reboot is disabled in configuration
- **THEN** the device keeps retrying reconnects indefinitely and never
  self-reboots

#### Scenario: Stalled firmware is not left to the network-level reboot

- **WHEN** the device is offline because its firmware has stalled and the
  connectivity watchdog no longer runs
- **THEN** recovery does not depend on the last-resort timer
- **AND** the liveness watchdog resets the device within its own timeout
