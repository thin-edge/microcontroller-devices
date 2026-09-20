# tedge-cert-renewal Specification

## Purpose
How the device keeps its identity valid over years: when it renews its certificate, how it authenticates that request, what happens when a renewal fails, and how an operator sees a fleet's certificate health before an expiry becomes an outage.

## Requirements
### Requirement: The device renews its certificate before it expires

When the certificate-renewal feature is built in, the client SHALL read the
expiry of its own certificate and renew it through the cloud's
re-enrollment endpoint once the remaining lifetime falls below a configured
margin. It SHALL authenticate the request with the token it holds for the
tenant, because the endpoint refuses a client certificate alone. It SHALL
keep the same key.

#### Scenario: The margin is reached

- **WHEN** a connected device's certificate has less remaining lifetime than
  the configured margin
- **THEN** the client requests a new certificate, stores it, and uses it for
  its next connection

#### Scenario: Well before the margin

- **WHEN** the certificate has more remaining lifetime than the margin
- **THEN** the client makes no renewal request

#### Scenario: No token yet

- **WHEN** the device has not obtained a token for the tenant
- **THEN** the client waits rather than sending a request that would be
  refused

### Requirement: A failed renewal never costs the working certificate

A renewal that fails at any step SHALL leave the existing certificate and
key in place and in use, and SHALL be retried. The client SHALL remain
connected throughout.

#### Scenario: The endpoint refuses the request

- **WHEN** the re-enrollment request fails or returns an error
- **THEN** the device keeps using its current certificate, stays connected,
  and tries again later

#### Scenario: The reply cannot be read

- **WHEN** the reply arrives but no certificate can be taken from it
- **THEN** nothing is stored, and the current certificate stays in use

### Requirement: An expiring certificate is visible and raises an alarm

The client SHALL publish the certificate's expiry and the days remaining as
twin data on every connect and after every renewal. When the remaining
lifetime falls below a second, shorter threshold and the certificate has
still not been renewed, the client SHALL raise a critical alarm naming the
expiry, and SHALL clear it once a renewal succeeds.

#### Scenario: An operator reviews a fleet

- **WHEN** devices are connected
- **THEN** each one's managed object carries the expiry date and the days
  remaining

#### Scenario: Renewal keeps failing

- **WHEN** renewal has not succeeded and the remaining lifetime falls below
  the alarm threshold
- **THEN** the device raises a critical alarm naming the expiry date

#### Scenario: Renewal succeeds after an alarm

- **WHEN** a renewal succeeds while the alarm is raised
- **THEN** the client clears the alarm and publishes the new expiry

### Requirement: Renewals are spread out across a fleet

The periodic check SHALL be spread within its interval, so that devices
deployed and enrolled together do not all renew at the same moment.

#### Scenario: A fleet enrolled on the same day

- **WHEN** many devices reach the renewal margin on the same day
- **THEN** their renewal requests are spread across the checking interval
  rather than arriving together

