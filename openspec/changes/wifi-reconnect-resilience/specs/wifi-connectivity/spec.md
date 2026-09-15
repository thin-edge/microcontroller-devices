## ADDED Requirements

### Requirement: Wi-Fi station bring-up

On Wi-Fi targets the firmware SHALL join the configured network in station mode
on boot and report the network as connected once an IPv4 address is assigned, so
protocol frontends can begin serving.

#### Scenario: Connect on boot

- **WHEN** the device boots with valid Wi-Fi credentials
- **THEN** it associates and obtains an IPv4 address
- **AND** the connected state becomes true and frontends start serving

### Requirement: Periodic connectivity watchdog

The firmware SHALL periodically evaluate connectivity independently of Wi-Fi/L4
management events. When it observes "not connected **or** no IPv4 address" for a
configurable number of consecutive checks, it SHALL trigger a reconnect. This
guarantees recovery even when no management event is delivered.

#### Scenario: Associated but no IP address

- **WHEN** the device is associated but has not obtained (or has lost) an IPv4
  address, and no further management event arrives
- **THEN** the watchdog detects the missing address within a few seconds
- **AND** forces a reconnect attempt (rather than remaining stranded)

#### Scenario: No spurious reconnects when healthy

- **WHEN** the device is connected with a valid IPv4 address
- **THEN** the watchdog takes no reconnect action and steady-state serving is
  undisturbed

### Requirement: Robust reconnect

Reconnection SHALL not dead-end on a stale association or a failed request. Before
re-issuing a connect request the firmware SHALL clear any existing association
(disconnect / state check) so the request cannot be a no-op, and it SHALL
reschedule another attempt if a connect request returns an error instead of
silently giving up.

#### Scenario: Reconnect while still associated

- **WHEN** a reconnect is attempted while the driver still considers itself
  associated (but connectivity is broken)
- **THEN** the firmware clears the association first and issues a fresh connect
- **AND** obtains a new IPv4 address

#### Scenario: Connect request fails

- **WHEN** a connect request returns an error
- **THEN** the firmware schedules another reconnect attempt (it does not stop
  retrying)

### Requirement: IP-loss detection

The firmware SHALL detect loss of its IPv4 address while still nominally connected
(e.g. a DHCP lease expiring without a clean disconnect event), treat it as a
disconnect, and initiate recovery.

#### Scenario: Silent lease loss

- **WHEN** the IPv4 address is lost while `connected` is still true
- **THEN** the firmware marks itself disconnected and begins reconnecting

### Requirement: Last-resort self-reboot

The firmware SHALL reboot itself if it remains offline for a configurable
prolonged period despite repeated reconnect attempts, so it recovers
autonomously. The timeout SHALL be Kconfig-configurable and the behavior SHALL be
possible to disable.

#### Scenario: Prolonged outage self-recovers

- **WHEN** the device has been offline continuously longer than the configured
  last-resort timeout despite reconnect attempts
- **THEN** it reboots itself and re-runs the normal bring-up sequence

#### Scenario: Reboot disabled

- **WHEN** the last-resort reboot is disabled in configuration
- **THEN** the device keeps retrying reconnects indefinitely and never self-reboots

### Requirement: Connectivity status indicator (LED)

On boards with a status LED, the firmware SHALL reflect connectivity on it so an
operator can tell at a glance whether the device is on the network: the LED SHALL
**blink while not connected** (booting/associating/reconnecting) and be **steady
once connected** (an IPv4 address is assigned and the frontend is serving). The
indicator SHALL use the board's LED alias, be enable/disable-able via Kconfig, and
be a no-op on boards that define no LED (and on native_sim).

#### Scenario: Disconnected shows a blinking LED

- **WHEN** the device is not connected (no IPv4 address / (re)connecting)
- **THEN** the status LED blinks
- **AND** an operator can distinguish a device-side network problem from a
  collector-side one without tools

#### Scenario: Connected shows a steady LED

- **WHEN** the device is connected with an IPv4 address and serving
- **THEN** the status LED is steady (not blinking)

#### Scenario: No LED present

- **WHEN** the board defines no status LED (or the indicator is disabled)
- **THEN** the firmware runs normally with no LED action (e.g. display-only boards
  continue to show status on the TFT)
