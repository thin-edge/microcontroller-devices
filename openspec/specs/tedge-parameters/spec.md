# tedge-parameters Specification

## Purpose
TBD - created by archiving change c8y-direct-parameters. Update Purpose after archive.
## Requirements
### Requirement: An application declares the settings the cloud may change

When parameters are built in, the client SHALL let the host application
declare a named set of parameters, each with a type, a default and the
limits of what it may hold. The client SHALL NOT expose a setting the
application did not declare.

The set's name SHALL be how the cloud addresses it: the state the client
reports, the schema an integrator registers, and the changes the cloud
sends SHALL all be identified by that one name, so that a device that
declares more than one set has each of them seen and changed separately.

#### Scenario: A device that has settings

- **WHEN** an application declares a parameter set and the device connects
- **THEN** the cloud shows the set's current values

#### Scenario: A device with more than one set

- **WHEN** an application declares two parameter sets and the cloud changes
  a value in one of them
- **THEN** only that set's value changes, and only that set is reported
  again

#### Scenario: Something that was never declared

- **WHEN** the cloud sends a value for a name the device does not declare
- **THEN** the device refuses the change and says which name it does not
  know

#### Scenario: A change for a set the device does not have

- **WHEN** the cloud sends a change addressed to a set name the device does
  not declare
- **THEN** the device refuses it naming that set, and no declared set is
  touched

### Requirement: The client offers its own settings the same way

The client SHALL offer its own settings as a parameter set of its own, so
that a device can be adjusted in the field whether or not its application
declares anything. That set SHALL be governed by every rule that governs an
application's: it is validated before it is applied, it is reported as
state, and an accepted value survives a restart.

The client SHALL only offer a setting that can take effect on a device that
is already running. A setting that is fixed when the image is built — one
that sizes a buffer, a stack or a thread — SHALL NOT be offered, because a
value the device cannot honour is worse than no value at all.

An application SHALL be able to decline the whole thing at build time, and
a set an application declares SHALL take precedence over a set of the same
name from the client, so that adding a client setting can never take a name
an application was already using.

#### Scenario: An application that declares nothing

- **WHEN** a device whose application declares no parameters connects
- **THEN** the cloud still shows the client's own settings, and can change
  them

#### Scenario: Turning off a capability in the field

- **WHEN** an operator turns off the client's remote access and the cloud
  then asks for a tunnel
- **THEN** the device refuses the tunnel and says that remote access is
  turned off, without anyone reflashing it

#### Scenario: Looking into a device that is misbehaving

- **WHEN** an operator raises the client's log level on a device already in
  the field
- **THEN** the device logs more from that point on, and still does so after
  it restarts

#### Scenario: A setting the running image cannot honour

- **WHEN** the client is built without a feature whose setting it would
  otherwise offer
- **THEN** that setting is absent from the set, and the cloud is not shown a
  value the device would ignore

### Requirement: The current values are visible without asking the device

The client SHALL publish the current value of every declared parameter as
device state on every connection and after every accepted change, so that
what the cloud shows is what the device is running.

#### Scenario: After a reboot

- **WHEN** a device restarts
- **THEN** the values the cloud shows are the ones the device is running,
  without anyone requesting them

### Requirement: A change is validated before any of it is applied

The client SHALL check every value in a change against its declaration —
type, range, length and allowed values — and SHALL apply the change only if
all of it passes. A change that fails SHALL leave every parameter as it was
and SHALL be reported as failed, naming the parameter and what was wrong
with it. The application SHALL be able to refuse a change it alone can
judge, and the device SHALL then keep its previous values.

#### Scenario: A value outside its range

- **WHEN** the cloud sets an interval above the declared maximum
- **THEN** the operation fails naming that parameter and its limit, and the
  device keeps running the value it had

#### Scenario: A change the application refuses

- **WHEN** a change passes validation but the application's hook rejects it
- **THEN** the operation fails with the application's reason, and the
  previous values are what the device runs and reports

#### Scenario: An accepted change

- **WHEN** a change passes validation and the application accepts it
- **THEN** the device runs the new values, reports them as its state, and
  the operation succeeds

### Requirement: Values survive a restart

An accepted value SHALL still be in force after the device restarts, without
the cloud sending it again. A stored value whose parameter the running
firmware no longer declares SHALL be discarded.

#### Scenario: A power cut

- **WHEN** a parameter is changed and the device loses power
- **THEN** it comes back running the changed value

#### Scenario: A firmware update that drops a parameter

- **WHEN** a new firmware no longer declares a parameter that had a stored
  value
- **THEN** the device drops it and reports only what it declares now

### Requirement: The device can produce the schema it validates against

The client SHALL be able to produce the JSON Schema of a declared set, so
that what is registered in the cloud is generated from the same declaration
the device validates against rather than written by hand. What it produces
SHALL be complete enough to register as it stands, without anyone having to
add the set's name or work out what the cloud needs in order to make the
values editable.

#### Scenario: Registering a set in the cloud

- **WHEN** an integrator needs the schema for a device's parameter set
- **THEN** the device can produce it, and it describes exactly the
  parameters, types and limits that the device enforces

