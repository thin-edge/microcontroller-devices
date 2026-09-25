## MODIFIED Requirements

### Requirement: Bounded, allocation-free request handling

The agent SHALL process each request within a fixed work buffer and a compile-time
bound on interface count, performing no per-request dynamic heap allocation. A
request that is malformed, unsupported, or too large to answer in one datagram
SHALL produce a valid SNMP error-status response (e.g. `tooBig`, `genErr`) or be
safely dropped, never crashing or leaking memory.

The MIB table SHALL describe each leaf by reference (its object kind and, for
table columns, its column and row) rather than by a stored copy of its full
OID; a leaf's OID SHALL be derived from constant prefixes when it is encoded or
compared. Per-request work buffers (walk cursors and response variable
bindings) SHALL refer to MIB leaves rather than holding OID copies, except for
the requested names the response must echo verbatim. The MIB, its walk order,
and every value returned SHALL be identical to the table-of-OIDs
implementation.

#### Scenario: Oversized GETBULK is bounded

- **WHEN** a `GetBulkRequest` asks for more repetitions than fit in one response
  datagram
- **THEN** the agent truncates the variable-binding list to what fits (or returns
  `tooBig`) and returns a well-formed response

#### Scenario: Malformed PDU does not crash

- **WHEN** a datagram with a malformed BER structure arrives on port 161
- **THEN** the agent discards it without crashing and remains able to serve
  subsequent valid requests

#### Scenario: Compact table walks identically

- **WHEN** a full `snmpwalk` of `1.3.6.1` and a `snmpbulkwalk` with
  max-repetitions 25 are run against the agent before and after the table
  change, with the same simulated values
- **THEN** both runs return the same OIDs in the same order with the same
  types, and the agent's static RAM is lower after the change

#### Scenario: GET of an OID between leaves

- **WHEN** a `GetRequest` names an OID that is a prefix of, or lies between,
  existing leaves
- **THEN** the agent returns `noSuchObject`/`noSuchInstance` exactly as before,
  and a `GetNextRequest` for the same OID returns the next leaf in
  lexicographic order
