# Task 1: what Parameter Update actually is

Notes taken against the `tedge-dev05` tenant and the
[tedge-parameter-plugin](https://github.com/thin-edge/tedge-parameter-plugin)
before any device code was written. Task 1.1 is answered here. Task 1.2 got
as far as the tenant allows and then stopped on a missing microservice
subscription — see *1.2, as far as it could be taken*.

## 1.1 What the tenant actually has

`c8y applications list` on `tedge-dev05.preprod.c8y.io` (tenant
`t297258657`) shows all four pieces:

| Name | Type | Context path |
|---|---|---|
| `digital-twin-manager` | HOSTED | `digital-twin-manager` |
| `dtm-plugins` | HOSTED | `dtm-plugins` |
| `dtm` | MICROSERVICE | `dtm` |
| `device-parameter` | MICROSERVICE | `device-parameter` |
| `Device Parameters Plugin` | HOSTED | `device-parameters-ui-plugin` |

**Appearing in that list is not the same as being subscribed.** Every one of
them is `availability=MARKET`, owned by the `management` tenant — that is
the market catalogue, not this tenant's subscriptions. Asking the services
themselves gives the real answer, and the two differ:

| Microservice | Subscribed? | Evidence |
|---|---|---|
| `dtm` | **yes** | `POST /service/dtm/definitions/properties` → `201` |
| `device-parameter` | **no** | `403 Microservice/Access Denied` — *"Microservice device-parameter is not subscribed by tenant t297258657"* |

So the Digital Twin Manager half works and a schema can be registered, but
the half that turns an operator's edit into a `c8y_ParameterUpdate_<set>`
operation is not there. This is exactly what the plugin's README warns
about: *"You may need to create a Cumulocity support ticket to request
access to the following microservices."* **It blocks task 1.2**, and it will
block the hardware verification in section 8 just as hard — a device can
report its set and nothing will ever be able to change it.

The roles are fine. An earlier reading of this said the user was missing
`ROLE_DIGITAL_TWIN_DEFINITIONS_*`; that was a revoked session token
returning 401 on everything, not a permission problem. With a fresh token
every DTM call works.

## 1.1 What the operation carries

Two shapes, and the important one is **not** what the design assumed.

### The fragment is named after the set

The plugin finds the set by looking for a fragment *prefix*, not a fixed
name ([`src/parameter_update.sh`][sh]):

```sh
get_parameter_type() {
    echo "$1" | jq -r '.operation | keys | .[] |
        select(. | startswith("c8y_ParameterUpdate_")) |
        sub("^c8y_ParameterUpdate_"; "")'
}
```

So a set called `pump` arrives as:

```json
{
  "id": "1234",
  "deviceId": "5678",
  "c8y_ParameterUpdate_pump": { "interval_s": 60, "auto_mode": false },
  "status": "PENDING"
}
```

The values are **typed JSON**, not strings — the schema registered in the
Digital Twin Manager gives them their types.

> **This corrects D6 and the spec, which both say the device handles a
> fragment called `c8y_ParameterUpdate`.** It has to match on the
> `c8y_ParameterUpdate_` prefix and take the set name from the suffix,
> which is also how one device can offer more than one set.

> **And this reading was itself half wrong — see *The shape, corrected*
> at the end of this note.** The suffixed fragment names the set but
> carries no values; `jq ".operation.\"$TYPE\""` above indexes the *bare*
> set name at the top level of the operation, not the suffixed fragment.
> It was there to be read correctly and was not.

### Over SmartREST, template 532

For a device on static templates the same change arrives flattened, with
the types spelled out one letter at a time:

```
532,someSerial,MyParameter,b,true,DeviceMaintainer.name,s,John Smithsky,DeviceMaintainer.contact,s,12312,Problems,,
```

Repeating triplets of *name, type, value*, where the type is `s` (string),
`n` (number), `b` (boolean) or empty (null), and the value is always a
string. Maximum 100 parameters per operation. Template 408 is the
counterpart the device can use to *report* a parameter update as an event.

Since `c8y-operation-ids` this client takes operations as JSON with their
id, so **532 is not needed** — the JSON fragment above is the path to
implement, and 408 is not needed either because the twin carries the state.

## 1.1 How the cloud learns the shape

A parameter set is a **property definition** in the Digital Twin Manager,
registered once per tenant by a human:

```sh
c8y api --raw POST /service/dtm/definitions/properties --template '{
  "identifier": "AutoUpdater",
  "jsonSchema": {
    "title": "Auto Updater",
    "description": "...",
    "type": "object",
    "properties": {
      "enabled":  { "type": "boolean", "default": false, "title": "enabled",  "order": 1 },
      "interval": { "type": "string", "enum": ["hourly","daily","weekly"], "title": "interval", "order": 2 }
    }
  },
  "contexts": ["asset", "event", "operation"]
}'
```

Three things this pins down for D5, the schema generator:

- **`identifier` is the set name**, and it is what the twin fragment and
  the operation fragment suffix are both named after. One name, three uses.
- **`contexts` must include `asset` and `operation`**, or the UI shows the
  values but will not let anyone edit them. The plugin also asks for
  `event`.
- Each property carries `title` and `order` beside the JSON Schema
  keywords, so the UI can render the fields in a sensible sequence. The
  generator should emit both — `order` from the table index, `title` from
  the declared name unless something better is declared.

Deleting one:

```sh
c8y api DELETE "/service/dtm/definitions/properties/AutoUpdater?contexts=asset,event,operation"
```

Updating one is documented as *not working* ("Maybe not all fields can be
updated"), so a changed declaration means delete-then-create. Worth a line
in the README.

## 1.1 How the values are reported

Exactly as D2 assumed — the twin fragment is named after the set:

```
te/device/main///twin/<set>   {"interval_s":30,"auto_mode":true}
```

The plugin's setup step publishes `{}` retained to that topic so the UI has
something to show before the first change, which suggests the client should
publish the set on connect even when every value is still its default.

## 1.2, as far as it could be taken

**What was done against the tenant**

- A mock device, `tedge-param-probe` (id `14215738`), carrying
  `c8y_SupportedOperations: ["c8y_ParameterUpdate_pump"]` and a `pump`
  fragment with the current values.
- The hand-written `pump` schema above, registered in the Digital Twin
  Manager — `201`, and still registered.
- **The generated shape was validated.** The exact bytes
  `tedge_params_schema()` emits were posted under a throwaway identifier
  and accepted with `201`, with `order`, `title`, `default`, `minimum`,
  `maximum`, `maxLength`, `enum`, `description` and `contexts` all
  preserved. The server adds `$schema` and `additionalProperties: false`
  itself, so the generator does not need to emit them. That is D5 and the
  "complete enough to register as it stands" requirement confirmed against
  the real API rather than against a test's expectations. The throwaway was
  deleted afterwards.

**What could not be done: the change itself.** Without the
`device-parameter` microservice there is nothing to turn an edit into an
operation, so the one question 1.2 exists to answer is still open. Creating
the operation by hand would only prove what this client does with a shape
this client invented, which is what the unit tests already do.

What it still has to settle, once the microservice is subscribed:

1. **Whole set or only the changed values?** The plugin passes whatever the
   fragment holds straight to the set's script without merging, which hints
   at *only the changed ones*, but the UI may well send everything it has.
   This decides whether validation runs over a patch or a whole set — D3
   currently assumes a patch.
2. Whether a value absent from the operation must keep its current value or
   be reset to its default.
3. What the UI does with a set whose schema it has but which the device has
   never reported, and whether a failed operation shows the reason string.
4. Whether `c8y_ParameterUpdate_<set>` is also what arrives when the tenant
   is on the newer Device Parameters UI plugin rather than the DTM screen.

[sh]: https://github.com/thin-edge/tedge-parameter-plugin/blob/main/src/parameter_update.sh

## Found on the way past: two Kconfig gaps that are not this change's

Neither is touched by `c8y-direct-parameters`; both were turned up while
trying to compile-check the new code, and both belong to whoever picks up
the build next.

### 1. The client selects ECDSA but no curve

`tedge-zephyr/Kconfig`'s `TEDGE_ENROLL_SELECTS` selects `PSA_WANT_ALG_ECDSA`
and the ECC key types. Anything that enables a builtin ECC key type turns on
`MBEDTLS_ECP_LIGHT`, and `ecp.h` then refuses to compile unless some
`MBEDTLS_ECP_DP_*_ENABLED` is set — which only happens if
`PSA_WANT_ECC_SECP_R1_256` (or another curve) is set too. The client never
sets one, so:

```
mbedtls/private/ecp.h:330: #error "Missing definition of MBEDTLS_ECP_MAX_BITS"
```

for *any* image that enables the Cumulocity auth path without the
application hand-picking a curve. The in-tree applications are unaffected
only because their board files set `CONFIG_PSA_WANT_ECC_SECP_R1_256=y`
themselves. Zephyr's own `modules/mbedtls/Kconfig.mbedtls` solves this with
`imply PSA_WANT_ECC_SECP_R1_256`; the client should do the same, as an
`imply` so an application can still choose a different curve. The same
applies to the TLS protocol version: the transport selects
`NET_SOCKETS_SOCKOPT_TLS` but neither `MBEDTLS_SSL_PROTO_TLS1_2` nor
`_TLS1_3`, so `sockets_tls.c` compiles against an mbedTLS with no SSL layer.

It arrived with `86c1b59` (2026-09-19), the commit that added the C8Y
transport and onboarding selects.

### 2. `samples/minimal` cannot be built as twister builds it

`tedge-zephyr/samples/minimal/sample.yaml` lists `native_sim/native/64` and
`esp32c6_devkitc/esp32c6/hpcore` with no `extra_args`, so twister builds the
bare `prj.conf` and hits exactly the error above on both. It needs
`extra_args: EXTRA_CONF_FILE=overlay-c8y.conf`, or a native_sim-safe variant
of that overlay — the current one sets `CONFIG_WIFI_ESP32=y`, and the
sample's `main.c` then calls `NET_REQUEST_WIFI_CONNECT`, which does not link
on native_sim.

Building the sample by hand works with the overlay the sample's own
`prj.conf` header already points at:

```sh
west build -b native_sim/native/64 tedge-zephyr/samples/minimal --pristine \
  -- -DEXTRA_CONF_FILE=<abs path>/tedge-zephyr/samples/minimal/overlay-c8y.conf
```

## Hardware verification (ESP32-C6, 2026-09-20)

Device `tedge-modbuse8f60afc320c` (`79211726`) on `tedge-dev05`, Modbus
application, **Core MQTT** — see the transport finding below for why not the
default endpoint.

Two sets are declared, which is the multi-set path the whole
`c8y_ParameterUpdate_<set>` correction exists for:

```
params: declared 'pump' with 3 parameters
params: declared 'tedge' with 3 parameters
twin pump:  {"interval_s":30,"telemetry":true,"measurement_type":"pump"}
twin tedge: {"log_level":"inf","health_interval_s":900,"required_interval_min":60}
```

| Task | Result |
|---|---|
| 8.1 the set appears with its values | both fragments landed in the managed object unchanged |
| 8.2 a change is applied and reported | `c8y_ParameterUpdate_tedge` → `2 value(s) changed`, twin republished, operation `SUCCESSFUL` |
| 8.3 a bad value fails, naming it | three refusals, below; twin untouched |
| 8.4 the application refuses | `pump: rolled back (measurement_type 'two words' cannot contain spaces)`, operation `FAILED` with that reason |
| 8.5 values survive a reboot | after a reset the device came up on `dbg`/`120`, the cloud-set values, not the declared defaults |
| 8.5 a removed parameter disappears | see below |
| 8.6 Core MQTT | everything above *is* Core MQTT |

**The refusals, verbatim from `failureReason`:**

```
health_interval_s: 999999 is outside 0..86400
tedge: this device does not declare 'nonsense'
log_level: 'loud' is not one of off err wrn inf dbg
```

**All-or-nothing, on real hardware.** A change carrying a valid
`required_interval_min: 30` *and* an invalid `log_level` left
`required_interval_min` at 60. The same held for the application's refusal:
a valid `interval_s: 45` travelling with the rejected `measurement_type` was
rolled back too.

**A parameter removed by a firmware update.** Rebuilt without
`CONFIG_TEDGE_HEALTH`, so `health_interval_s` left the declaration, and
reflashed:

```
params: declared 'tedge' with 2 parameters
params: forgetting 'tedge/param/tedge/health_interval_s', which nothing declares now
twin tedge: {"log_level":"dbg","required_interval_min":60}
```

`log_level` kept the value the cloud had set, and the stored value for the
dropped parameter was deleted. **The managed object also lost the key** —
so Cumulocity replaces a fragment published through
`inventory/managedObjects/update/<id>` wholesale rather than deep-merging
it. That had been an open risk in D4 (a removed parameter lingering in the
operator's view) and it turns out not to exist.

### The transport finding: twin data does not reach inventory on MQTT Service

The first image was built on the default endpoint, the **MQTT Service**
(`:9883`). Everything on the device worked — both sets were declared and
`twin tedge: {...}` was logged — but **nothing reached the managed
object**. `tedge_Agent` in the inventory still read
`"transport":"c8y-core-mqtt"` from an earlier run while the device was
publishing `"c8y-mqtt-service"`, two minutes and more after connecting.

The cause is in `publish_twin_impl()` and is not specific to parameters:

- on **MQTT Service** it publishes to the free-form topic
  `te/device/<id>///twin/<fragment>`. Cumulocity's MQTT Service accepts it
  as a broker would, but nothing maps `te/...` into inventory — on Linux
  that mapping is thin-edge.io's mapper, and there is no mapper here.
- on **Core MQTT** it publishes `inventory/managedObjects/update/<id>`,
  which Cumulocity does understand. That is why everything above works.

So D2 — "the twin is the operator's view" — holds only on Core MQTT with
this client, and MQTT Service is the default endpoint. This affects every
twin fragment the client publishes (`tedge_Agent`, `tedge_Certificate`,
`tedge_RemoteAccess`, and now the parameter sets), so it is a pre-existing
gap that parameters merely make obvious: a device on the default transport
would declare parameters an operator could never see. It wants its own
change.

### Footprint (task 8.7)

ESP32-C6, Modbus application, Core MQTT, telemetry and health on. Same
overlays throughout; only the parameter options differ.

| Build | `.text` | Cost |
|---|---|---|
| `CONFIG_TEDGE_PARAMETERS=n` | 724,208 | — |
| parameters on, `..._SCHEMA=n` | 731,552 | **+7,344 B** |
| parameters on, schema printer and shell | 732,912 | **+8,704 B** |

So the mechanism itself — declaration, validation, storage, the twin, the
operation path and the client's own set — is **7.2 KB**, and the schema
printer with its two shell commands adds **1.3 KB** on top. The printer is
only needed until the sets are registered in the tenant, so an image that
is past that point can turn it off and keep the rest.

RAM is what the declaration asks for: one current value per parameter
(bounded by `CONFIG_TEDGE_PARAMETERS_MAX`, 16 by default) plus one heap
allocation per string parameter, sized by the declaration and made once at
startup.

### Both sets registered from the device's own output

With the client flashed, the schemas were taken from the device rather than
written by hand — which is what D5 exists for:

```
uart:~$ tedge params list
pump (3 parameters)
tedge (3 parameters)
uart:~$ tedge params schema tedge
{"identifier":"tedge","jsonSchema":{...},"contexts":["asset","event","operation"]}
```

Both were posted to `/service/dtm/definitions/properties` unchanged and
accepted (`201`). The hand-written `pump` definition from task 1.2 was
deleted first and replaced with the generated one, so nothing in the tenant
is now transcribed by hand. The registered properties match the device's
twin exactly:

| Set | Registered properties | Twin on the device |
|---|---|---|
| `tedge` | `log_level`, `health_interval_s`, `required_interval_min` | same three |
| `pump` | `interval_s`, `telemetry`, `measurement_type` | same three |

**The definitions being there is not the same as the UI being able to edit
them.** That still needs the `device-parameter` subscription: the Digital
Twin Manager can render a set from its definition, but the thing that turns
an operator's edit into a `c8y_ParameterUpdate_<set>` operation is the
microservice, and it is not subscribed.

## The shape, corrected (2026-09-20, after archiving)

A real parameter change from the tenant failed on the device with
`tedge: no values to change`. The operation, verbatim:

```json
{
  "id": "214989",
  "deviceId": "79211726",
  "status": "FAILED",
  "failureReason": "tedge: no values to change",
  "description": "Update parameter 'tedge'",
  "tedge": {
    "required_interval_min": 30,
    "log_level": "dbg",
    "health_interval_s": 900
  },
  "c8y_ParameterUpdate": {},
  "c8y_ParameterUpdate_tedge": {}
}
```

Three things this settles, two of which had been guessed wrong:

1. **`c8y_ParameterUpdate_<set>` is an empty marker.** It says which set is
   being changed and carries nothing. There is a bare `c8y_ParameterUpdate`
   marker beside it.
2. **The values are a top-level fragment named after the set.** Searching
   for `"<set>":` cannot match the marker, because the character before the
   name there is an underscore rather than the opening quote.
3. **It is the whole set, not a patch.** All three values travelled although
   only `required_interval_min` was edited — `log_level` and
   `health_interval_s` came along at their current values. That answers the
   open question in design.md, and it is why "store only what changed"
   matters: without it every edit would write every key to flash.

The plugin's `jq ".operation.\"$TYPE\""` says exactly this, and the first
reading of it took `$TYPE` to be indexing the suffixed fragment rather than
the bare set name. The evidence was there and was misread; only a real
operation caught it.

**The fix** takes the set name from the marker's suffix as before, then
reads the values from the fragment named after the set, falling back to the
marker's contents when the named fragment is missing or empty — which is
how an operation built by hand through the REST API carries them, and how
every change in the hardware verification above was driven.

Replaying the exact operation above against the fixed firmware:

```
operation c8y_ParameterUpdate_tedge (216317)
params: tedge: 3 value(s) changed
twin tedge: {"log_level":"dbg","health_interval_s":900,"required_interval_min":30}
```

`SUCCESSFUL`, and the managed object agrees.

**What this says about the verification above.** Sections 8.2–8.5 all drove
changes through `POST /devicecontrol/operations` with the values inside the
suffixed fragment — a shape this client invented, and which the fallback
now keeps working. The validation, rollback, storage and twin behaviour
they proved is unaffected: only the extraction was wrong. But it is a
reminder that a self-built payload verifies the device, not the contract.

## Template 117 creates an availability, it does not update one

Changing `required_interval_min` moves the parameter and the twin, but
`c8y_RequiredAvailability.responseInterval` on the managed object does not
follow. That is Cumulocity's behaviour, not a defect here: template `117`
*creates* `c8y_RequiredAvailability` and is ignored once the managed object
has one. Any device that has connected with this client already has one,
set on its first connect from `CONFIG_TEDGE_REQUIRED_INTERVAL_MIN`.

So the parameter is worth having — it is what a fresh device registers
with, and it records the intent — but on a device already in the fleet the
availability window is a cloud-side edit. The parameter's help text and the
README both say so now, because an operator who changes it and watches the
availability card is otherwise left wondering.

Worth considering in a later change: on Core MQTT the client already
publishes twin fragments through `inventory/managedObjects/update/<id>`,
which *does* update an existing fragment. Sending
`{"c8y_RequiredAvailability":{"responseInterval":N}}` that way would make
the parameter effective on a device that already has one. It was left alone
here rather than widening an archived change.

### And one real defect, found while chasing it

`tedge_self_params.c` used `snprintf()` without including `<stdio.h>`, so
it was compiled against an implicit declaration. It happened to work on
this ABI, and the build said so in a warning that went unread because the
build output was being filtered to `error:` only. Fixed, and the module now
compiles with no warnings of its own.
