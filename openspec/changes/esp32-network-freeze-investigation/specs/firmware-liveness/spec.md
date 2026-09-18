## ADDED Requirements

### Requirement: Stall detection and self-recovery

When the liveness watchdog is enabled, the firmware SHALL watch the system
workqueue and every protocol-serving thread for progress. If any of them makes
no progress for longer than a configurable timeout, the firmware SHALL reset
the device. Each watched context SHALL feed the watchdog only when it has
actually made progress, so a stalled context is detected even if the rest of
the system still runs. A hardware watchdog SHALL back this up, so the device
also resets if a lockup stops the software watchdog itself. Enabling the
watchdog and its timeout SHALL be set in Kconfig.

#### Scenario: System workqueue blocked

- **WHEN** a work item on the system workqueue blocks and the workqueue stops
  making progress for longer than the configured timeout
- **THEN** the device resets
- **AND** it runs the normal bring-up and serves again without manual action

#### Scenario: Protocol thread stuck

- **WHEN** a protocol server thread (OPC-UA, Modbus or SNMP) stops making
  progress for longer than the configured timeout while the rest of the system
  keeps running
- **THEN** the device resets

#### Scenario: Hard lockup

- **WHEN** the firmware locks up so that the software watchdog can no longer run
  (for example, interrupts are masked)
- **THEN** the hardware watchdog resets the device

#### Scenario: Healthy device is never reset

- **WHEN** the device runs normally for an extended period, including Wi-Fi
  association, DHCP and reconnects after an access-point restart
- **THEN** the liveness watchdog causes no reset

#### Scenario: Watchdog disabled

- **WHEN** the liveness watchdog is disabled in configuration
- **THEN** the firmware registers no watchdog channels and does not arm the
  hardware watchdog
- **AND** otherwise behaves as before

### Requirement: Reset cause reporting

At boot, the firmware SHALL log why the previous reset happened. If the
liveness watchdog caused it, the log SHALL also say which watched context
stopped and how long the device had been running. That record SHALL survive
the reset without needing flash storage.

#### Scenario: After a liveness reset

- **WHEN** the device boots after the liveness watchdog reset it because the
  system workqueue stalled
- **THEN** the boot log says the reset came from the liveness watchdog
- **AND** names the system workqueue as the stalled context and gives the uptime
  at which it stalled

#### Scenario: After a hardware watchdog reset

- **WHEN** the device boots after the hardware watchdog fallback reset it
- **THEN** the boot log reports a hardware watchdog reset, distinct from a
  power-on, brownout or software reset

#### Scenario: After power-on

- **WHEN** the device boots from power-on
- **THEN** the boot log reports a power-on reset and no stalled context

### Requirement: Liveness and resource health diagnostics

When diagnostics are enabled, the firmware SHALL periodically log a health line
from a thread that does not run on the system workqueue. The line SHALL show:
- how long ago each watched context last made progress;
- which of the firmware's own work items, if any, is currently running on the
  system workqueue;
- free heap;
- free network packets and buffers;
- the Wi-Fi association state and signal strength.

When a watched context stops making progress, the diagnostics SHALL also log
that thread's state and the kernel object it is waiting on. When diagnostics
are disabled, they SHALL not be compiled into the firmware.

#### Scenario: Workqueue blocked with diagnostics on

- **WHEN** the system workqueue blocks inside one of the firmware's work items
  and diagnostics are enabled
- **THEN** successive health lines show the workqueue's time since last progress
  growing and name the work item that is running
- **AND** the thread state and the object it waits on are logged

#### Scenario: Resource exhaustion trend

- **WHEN** free network buffers or free heap fall toward zero before an outage
- **THEN** the health lines logged before the outage show that decline

#### Scenario: Diagnostics disabled

- **WHEN** diagnostics are disabled in configuration
- **THEN** the image contains no diagnostics thread or health reporting, and
  flash and RAM use are the same as without the feature

### Requirement: Diagnostic output survives a stall

When diagnostics are enabled, log messages SHALL be written out as they are
logged, not held in a buffer for later, so everything logged before a stall
reaches the console even if the logging thread never runs again.

#### Scenario: Last messages before a stall are visible

- **WHEN** the firmware logs a message and then stalls, so the logging thread
  never runs again
- **THEN** that message has already appeared on the console
