# tedge-c8y-connection Specification

## Purpose
The client's single MQTT session to Cumulocity: which endpoint matches which authentication, how a session starts so no operation is missed, how it recovers from network loss without leaking, and the token it keeps for HTTPS and WebSocket calls.

## Requirements
### Requirement: One MQTT session to Cumulocity, on the endpoint that matches the authentication

The client SHALL hold one MQTTS session to Cumulocity: the MQTT Service (port
9883) with certificate authentication, or Core MQTT (port 8883) with basic
authentication or when the Core MQTT endpoint is selected. It SHALL validate
the server certificate against the configured trust anchors, and SHALL only
connect once the realtime clock is valid (set by the application or by the
client's SNTP query). It SHALL subscribe with one topic filter per SUBSCRIBE
packet, and SHALL NOT subscribe to wildcard free-form topics.

#### Scenario: Certificate device connects

- **WHEN** an enrolled device has an IPv4 address and a valid clock
- **THEN** the client connects to port 9883 with mutual TLS and reaches the
  connected state

#### Scenario: Clock not yet set

- **WHEN** the network is up but the realtime clock predates the build
- **THEN** the client sets it with SNTP before opening the TLS session

### Requirement: The session starts by creating the device

After each CONNACK, the client SHALL first publish the device-creation message,
then subscribe to the operation topics, then publish the supported operations,
the required interval and its twin data. It SHALL treat a refused subscription
(return code 0x80) as temporary and retry it, so a newly created device does not
miss operations in its first session.

#### Scenario: First session of a bootstrap-registered device

- **WHEN** a device connects as a device user for the first time and the
  broker refuses its subscriptions because the device doesn't exist yet
- **THEN** the client retries the subscriptions after the device-creation
  message has been processed, and operations sent in that session arrive

### Requirement: The client recovers from connection loss by itself

The client SHALL reconnect after any loss of the session (network down, broker
close, keepalive timeout) with an exponential back-off that starts at 3 seconds,
is capped by a Kconfig maximum, includes jitter, and resets after a stable
period. It SHALL stop connecting while the application's network is down and
resume when an address is available again. After each disconnect, the TLS heap
in use and the TCP contexts held by the client SHALL return to their
pre-connect values.

#### Scenario: Wi-Fi drops for a minute

- **WHEN** the access point disappears for 60 seconds and returns
- **THEN** the client reports waiting for network, reconnects once the
  application's network is back, and its TLS heap and TCP context usage match
  the values before the drop

#### Scenario: Broker closes the session

- **WHEN** the broker closes the session repeatedly
- **THEN** the client waits increasing intervals between attempts, never less
  than 3 seconds

### Requirement: A duplicate client ID is reported

The client SHALL log a warning naming a possible duplicate client ID when the
broker closes three consecutive sessions within 10 seconds of their CONNACK.

#### Scenario: Two devices share an external ID

- **WHEN** another client connects with the same client ID and the broker keeps
  closing this device's session shortly after it connects
- **THEN** the log says that another client may be using this ID

### Requirement: The client keeps a current token for HTTPS and WebSocket calls

A client with certificate authentication SHALL request a JWT after every
connect and before the current one expires, and keep the latest one for the
module's HTTPS and WebSocket features. A client with basic authentication
SHALL NOT request one.

#### Scenario: Token after connect

- **WHEN** a certificate device connects
- **THEN** it has a token within seconds, and a fresh one before the previous
  token's one-hour lifetime ends

