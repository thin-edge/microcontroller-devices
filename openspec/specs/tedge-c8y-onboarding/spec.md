# tedge-c8y-onboarding Specification

## Purpose
How a device gets the identity and credentials it needs for Cumulocity: enrolling itself with the Cumulocity CA (device-generated key, one-time password and registration URL), the bootstrap-user fallback, and where that state is stored.

## Requirements
### Requirement: The device enrolls itself with the Cumulocity CA

When built with Cumulocity CA authentication, the client SHALL obtain its
device certificate itself. It SHALL generate a persistent P-256 key in PSA
secure storage, generate a one-time password with the PSA random generator,
build a PKCS#10 request whose common name is the external ID and sign it inside
PSA. It SHALL then poll the tenant's EST `simpleenroll` endpoint with HTTP Basic
`<external ID>:<one-time password>` until a certificate is issued. It SHALL
store the certificate, delete the one-time password, and use the key and
certificate for mutual TLS. An enrolled device SHALL NOT enroll again after a
reboot or an application update.

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
overridable at runtime through the public API, persisted in settings.

#### Scenario: Tenant host overridden at runtime

- **WHEN** the application sets a tenant host through the API before the
  device is enrolled
- **THEN** enrollment and the MQTT session use that host, and it is still used
  after a reboot

