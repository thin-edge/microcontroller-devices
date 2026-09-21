# tedge-c8y-onboarding Specification

## Purpose
How a device gets the identity and credentials it needs for Cumulocity: enrolling itself with the Cumulocity CA (device-generated key, one-time password and registration URL), the bootstrap-user fallback, and where that state is stored.
## Requirements
### Requirement: The device enrolls itself with the Cumulocity CA

When built with Cumulocity CA authentication, the client SHALL obtain its
device certificate itself. It SHALL generate a persistent P-256 key in PSA
secure storage, obtain a one-time password — using the password supplied
through `tedge_set_enroll_otp()` when there is one, otherwise generating one
with the PSA random generator — build a PKCS#10 request whose common name is
the external ID and sign it inside PSA. It SHALL then poll the tenant's EST
`simpleenroll` endpoint with HTTP Basic `<external ID>:<one-time password>`
until a certificate is issued. It SHALL store the certificate, delete the
one-time password, and use the key and certificate for mutual TLS. An enrolled
device SHALL NOT enroll again after a reboot or an application update.

#### Scenario: First boot of an unregistered device

- **WHEN** a device with no stored certificate starts and the operator has not
  registered it yet
- **THEN** the client enters the awaiting-registration state, polls
  `simpleenroll` at the configured interval with back-off, and does not open
  an MQTT session

#### Scenario: Operator registers the device

- **WHEN** the operator registers the external ID with the one-time password
  while the device is polling
- **THEN** the next poll returns a certificate, the client stores it, and
  connects with mutual TLS without a reboot

#### Scenario: Enrolled device restarts

- **WHEN** an enrolled device reboots, or its application image is updated
- **THEN** it connects with the stored key and certificate and does not create
  a new one-time password

#### Scenario: Device pre-registered by a provisioning server

- **WHEN** a device is started with a tenant host and a one-time password that
  a provisioning server already registered with Cumulocity
- **THEN** the first `simpleenroll` attempt returns a certificate, and the
  device connects without an operator touching the Cumulocity UI

### Requirement: The registration URL is handed to the application

The client SHALL provide the registration URL (tenant host, external ID and
one-time password pre-filled) through its public API while the device is
awaiting registration. It SHALL report the awaiting-registration state through
the state hook, so the application can show the URL. The client SHALL NOT log
the one-time password at log levels above debug.

#### Scenario: Application shows the URL

- **WHEN** the state hook reports awaiting registration and the application
  asks for the registration URL
- **THEN** it receives the URL, and the API returns "already enrolled" once the
  certificate is stored

### Requirement: Bootstrap credentials as a fallback

When built with bootstrap authentication, the client SHALL connect to Core MQTT
with the tenant's bootstrap credentials, request device credentials, and store
the tenant, user name and password it receives. It SHALL then connect as the
device with basic authentication. Stored credentials SHALL be reused after a
reboot. The client SHALL NOT log the bootstrap or device password.

#### Scenario: Device request accepted

- **WHEN** the operator accepts the device's registration request
- **THEN** the client receives and stores the device credentials, disconnects
  the bootstrap session, and connects as the device

### Requirement: Credentials and settings stay in the module's namespace

Onboarding state SHALL live under the `tedge/` settings subtree, the key at a
configurable PSA key ID, and the TLS credentials at tags from the module's
configurable tag base. The tenant host SHALL default to a Kconfig value and be
overridable at runtime through the public API, persisted in settings. The
one-time password SHALL likewise be settable at runtime through the public API
and persisted in the same subtree.

No component outside the module SHALL write to the `tedge/` subtree; an
application that obtains onboarding data elsewhere SHALL pass it in through the
public API rather than writing the module's keys directly.

#### Scenario: Tenant host overridden at runtime

- **WHEN** an application sets a tenant host through the API before the
  device is enrolled
- **THEN** enrollment and the MQTT session use that host, and it is still used
  after a reboot

#### Scenario: Provisioned data reaches the module through the API

- **WHEN** a provisioning image has stored a tenant host and one-time password
  outside the `tedge/` subtree, and the application reads them at boot
- **THEN** the application passes them through `tedge_set_c8y_url()` and
  `tedge_set_enroll_otp()`, and the module's own keys were not written by
  anything but the module

### Requirement: The one-time password can be supplied by the application

The client SHALL expose `tedge_set_enroll_otp()`, which stores a one-time
password issued outside the device, of any length from 1 to 64 printable ASCII
characters — for example by a zero-touch provisioning
server that has already registered the device with Cumulocity. It SHALL be
called before `tedge_start()`, SHALL persist the value in settings, and the
stored value SHALL take precedence over generating one. The client SHALL NOT
log the supplied password above debug level, and SHALL delete it once a
certificate has been issued, exactly as it does for a password it generated
itself.

The client SHALL remain unaware of how the application obtained the password;
it SHALL NOT depend on any provisioning transport.

#### Scenario: Externally issued password is used

- **WHEN** the application calls `tedge_set_enroll_otp()` with a password the
  operator already registered in Cumulocity, and then starts the client
- **THEN** the device enrolls with that password on its first `simpleenroll`
  attempt and never generates one of its own

#### Scenario: Supplied password is cleared after enrollment

- **WHEN** a device that was given a password receives its certificate
- **THEN** the stored password is deleted, and a reboot does not re-enroll

#### Scenario: Supplied password of a different length

- **WHEN** the supplied password is not the 32 characters the device would
  generate itself
- **THEN** the client still uses it, rather than discarding it and generating
  its own

#### Scenario: Credential never truncated

- **WHEN** the external ID and password together are too long for the
  enrollment request's credential
- **THEN** enrollment fails with an error, rather than sending a shortened
  credential that cannot authenticate

#### Scenario: A supplied password is never revealed

- **WHEN** the application asks for the registration URL while the device is
  awaiting registration with a supplied password — before or after a reboot
- **THEN** `tedge_registration_url()` returns `-EACCES` and yields no URL,
  since the password may have reached the device sealed end to end and whoever
  issued it has already registered the device

#### Scenario: Re-onboarding an enrolled device

- **WHEN** a device that already holds a certificate — for example from an
  earlier enrollment with another tenant — is given a new password through
  `tedge_set_enroll_otp()`
- **THEN** the old certificate is discarded (the key pair is kept) and the
  device enrolls with the new password, instead of presenting the old
  certificate to a tenant that refuses it

#### Scenario: No password supplied

- **WHEN** the application does not call `tedge_set_enroll_otp()`
- **THEN** the client generates its own password and the registration URL flow
  is unchanged

