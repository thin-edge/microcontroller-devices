## ADDED Requirements

### Requirement: An application declares the settings the cloud may change

When parameters are built in, the client SHALL let the host application
declare a named set of parameters, each with a type, a default and the
limits of what it may hold. The client SHALL NOT expose a setting the
application did not declare.

#### Scenario: A device that has settings

- **WHEN** an application declares a parameter set and the device connects
- **THEN** the cloud shows the set's current values

#### Scenario: Something that was never declared

- **WHEN** the cloud sends a value for a name the device does not declare
- **THEN** the device refuses the change and says which name it does not
  know

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
the device validates against rather than written by hand.

#### Scenario: Registering a set in the cloud

- **WHEN** an integrator needs the schema for a device's parameter set
- **THEN** the device can produce it, and it describes exactly the
  parameters, types and limits that the device enforces
