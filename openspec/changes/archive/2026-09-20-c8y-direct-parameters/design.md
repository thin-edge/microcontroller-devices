## Context

The client can be operated, can report, and can explain itself. What it
cannot do is be *configured*: `tedge_register_config_type()` has no
definition, and the shape it declares — a snapshot reader and a chunked
writer — is a file-shaped answer on a device with no files.

Cumulocity offers two mechanisms:

| | Configuration files (`c8y_Configuration`) | Parameters (`c8y_ParameterUpdate`) |
|---|---|---|
| What travels | a file, uploaded and downloaded over HTTPS | the changed values, inside the operation |
| What an operator sees | a blob to download and read | typed fields, with the current values |
| Validation | none until the device chokes | a JSON Schema, plus whatever the device checks |
| What the device needs | the HTTP path both ways, somewhere to put a file | the twin it already publishes |
| In thin-edge.io | `tedge-configuration-plugin` | [`tedge-parameter-plugin`](https://github.com/thin-edge/tedge-parameter-plugin) |

For a microcontroller the right-hand column wins on every row, so this
change implements parameters and removes the file-shaped API rather than
leaving both.

What the plugin does on Linux, and what this client mirrors: a **parameter
set** is a named group with a JSON Schema registered in Cumulocity's Digital
Twin Manager; the current values live in the device twin under the set's
name; a change arrives as `c8y_ParameterUpdate` (SmartREST template 532) and
the device applies it and updates the twin.

## Goals / Non-Goals

**Goals:** an application declares typed parameters in a few lines; an
operator sees the current values and changes one; a bad value is refused
with a reason and changes nothing; values survive a reboot; no filesystem,
no HTTP, no extra TLS session.

**Non-Goals:** configuration files, nested structures, registering the
schema in the tenant, a general settings API for the application's own use.

## Decisions

### D1: A parameter is declared, not discovered

```c
static const struct tedge_parameter pump_params[] = {
        TEDGE_PARAM_INT("interval_s", 30, 5, 3600,
                        "Seconds between measurements"),
        TEDGE_PARAM_BOOL("auto_mode", true, "Run the pump automatically"),
        TEDGE_PARAM_ENUM("profile", "normal", ("normal", "quiet", "boost"),
                         "Operating profile"),
        TEDGE_PARAM_STRING("site", "", 32, "Where this device is"),
};

tedge_declare_parameters("pump", pump_params, ARRAY_SIZE(pump_params),
                         on_parameters_changed, NULL);
```

The table is `const` in the application's flash: the client holds a pointer
to it, the current values, and nothing else. Types are the four an MCU
actually needs; a value the application wants structured is its own to
encode, and the README says so.

*Alternative: discover parameters from the settings subtree.* Rejected:
nothing would state a type, a range or a default, so nothing could be
validated or shown properly.

### D2: The twin is the operator's view

On every connect, and after every accepted change, the client publishes the
set as one twin fragment named after it:

```
te/device/<id>///twin/pump  {"interval_s":30,"auto_mode":true,"profile":"normal","site":""}
```

Twin data is state, not events (the rule the client already follows), so a
reboot never leaves a stale view. On Core MQTT the same values go out as an
inventory update, as the other fragments do.

### D3: A change is all-or-nothing, and validated against the declaration

The operation carries the set's values — in practice the whole set, since
that is what Cumulocity sends (D6). The client checks every
one against its declaration — type, range, length, allowed values — before
storing any of them. If one fails, **nothing is applied** and the operation
fails naming the parameter and the limit it broke. A half-applied
configuration is how a device ends up in a state nobody can reproduce.

The application's hook is called once, after the values are stored, with the
set. It may still refuse (a combination only it understands), and then the
client rolls back to the stored values and fails the operation with the
hook's reason.

### D4: Values live in the client's settings, one key each

`tedge/param/<set>/<key>`, written only when a value actually changes, so an
operator pressing "save" with nothing changed costs no flash. On startup the
stored values replace the declared defaults, and anything stored that is no
longer declared is dropped (a firmware update may remove a parameter).

### D5: The device prints its own schema

The schema in the tenant must match the declaration in the firmware, and
writing it twice by hand is how they drift. With
`CONFIG_TEDGE_PARAMETERS_SCHEMA` the client can print the JSON Schema of a
set — on the console, through the `tedge params schema` shell command — for
whoever registers it in the Digital Twin Manager. It is generated from the
same table the validation uses, so the two cannot disagree.

What it prints is the whole body of the registration call, not a bare
schema, because that is what the person pasting it needs:

```json
{"identifier":"pump",
 "jsonSchema":{"type":"object","title":"pump","properties":{
   "interval_s":{"type":"integer","title":"interval_s","order":1,
                 "default":30,"minimum":5,"maximum":3600,
                 "description":"Seconds between measurements"}}},
 "contexts":["asset","event","operation"]}
```

Three details that are not optional:

- **`identifier` is the set name**, and it is the same string as the twin
  fragment (D2) and the operation fragment's suffix (D6). One name, three
  uses — so the client validates it once, at declaration.
- **`contexts` must contain `asset` and `operation`**, or the UI shows the
  values but refuses to let anyone edit them.
- **`title` and `order`** sit beside the JSON Schema keywords so the UI can
  lay the fields out; `order` comes from the position in the declared
  table, `title` from the parameter's name.

Registering an already-registered identifier again is documented as not
working, so a changed declaration means delete then create. The README says
so, with the `DELETE` call.

### D6: The operation is answered like every other one

The operation arrives as JSON with its id (`c8y-operation-ids`), is turned
into a line the dispatcher understands, and is completed with `506,<id>` or
failed with `505,<id>,<reason>`. It runs on the client thread: validating
and storing a handful of values is microseconds, and there is nothing to
wait for.

**The fragment is named after the set, and it is empty.** It is not a fixed
`c8y_ParameterUpdate`; the set's name is the suffix, and the client matches
on the prefix and takes the name from what follows it. But that fragment
carries no values — it is a marker saying *which set*. The values arrive in
a separate top-level fragment named after the set, and they are the **whole
set**, not only what an operator touched:

```json
{"id":"214989","deviceId":"79211726",
 "description":"Update parameter 'tedge'",
 "tedge":{"log_level":"dbg","health_interval_s":900,
          "required_interval_min":30},
 "c8y_ParameterUpdate":{}, "c8y_ParameterUpdate_tedge":{}}
```

That is how one device can offer more than one set, and it is why the
dispatcher cannot key on an exact fragment name the way it does for every
other operation. The values are typed JSON — the schema registered in the
tenant is what gives them their types — so nothing has to be parsed out of
strings.

An operation built by hand through the REST API more naturally carries the
values inside the suffixed fragment, so the client accepts that too when
the named fragment is missing or empty.

See [findings-mechanism.md](./findings-mechanism.md); this shape was
corrected after a real operation from the tenant failed against the first
reading of it.

The SmartREST form of the same thing (static template `532`) flattens it
into *name, type, value* triplets with every value a string. This client
does not need it: it has taken operations as JSON since
`c8y-operation-ids`. Template `408` reports a parameter change as an event,
which this client also does not need, because the twin carries the state
(D2).

### D8: The client declares a set for itself

Everything in this module is configured at build time, which is the right
default on a microcontroller: an option that cannot change cannot surprise
anyone, and it costs no flash. But a handful of those choices are ones
somebody wants to revisit on a device that is already in the field and
misbehaving, and reflashing a fleet to turn up a log level is not a plan.

So the client declares a set of its own, `tedge`, through the same API an
application uses:

| Parameter | What it changes | Where it takes effect |
|---|---|---|
| `log_level` | how much the client logs | the logging filter, at once with `CONFIG_LOG_RUNTIME_FILTERING`, otherwise next boot |
| `health_interval_s` | how often it reports its own health | `tedge_health.c`, next time it is due |
| `required_interval_min` | the window Cumulocity calls it offline by | republished as `117,<n>` on change |
| `remote_access` | whether the cloud may tunnel in | checked before a tunnel is opened |

**Only what a running device can honour.** A setting that sizes a buffer, a
stack or a thread is fixed once the image is linked, and offering it would
show an operator a value the device quietly ignores — worse than offering
nothing. So the set holds only values that are read where they are used,
and each entry above names that place. Whatever the running image does not
have is not in the set either: no health feature, no `health_interval_s`.

**The application still wins.** The client declares its set *after* the
application has declared its own, so a name an application already took
stays the application's — adding a client setting later can never quietly
take one over.

*Alternative: expose these as custom operations, or as another transport
message.* Rejected: they are settings, they want validating, reporting and
remembering, and that machinery now exists. A second mechanism for the same
shape of thing is how two mechanisms drift.

*Alternative: leave them build-time only.* That is what the client did
before, and it is why a device that logs too little has to be recovered
from a bench rather than from a desk.

`CONFIG_TEDGE_PARAMETERS_SELF` turns the whole thing off for an image that
wants nothing of it.

### D7: The file-shaped API goes

`tedge_register_config_type()`, `tedge_config_reader_t` and
`tedge_config_writer_t` are removed from the public header rather than left
as `-ENOTSUP` stubs for a feature this client is not going to grow. The
header says what replaced them, and the archived proposal says why.

## Risks / Trade-offs

- [The tenant may not have Parameter Update] → it needs the Digital Twin
  Manager and the device-parameter microservice, so the first task is to
  confirm the feature exists in the tenant and capture what `532` actually
  delivers, before anything else is written.
- [The schema lives in two places] → generated from the declaration, never
  written twice.
- [A parameter removed by a firmware update] → stored values with no
  declaration are dropped at startup, and the twin shows what the running
  image actually has.
- [An application that wants structure] → out of scope, stated plainly, with
  the advice to encode it in a string and own the parsing.

## Migration Plan

- Nothing uses the configuration API today (it has no definition anywhere),
  so removing it breaks no build.
- An application that adopts parameters gets its defaults on first boot and
  keeps whatever an operator sets afterwards.

## Open Questions

- ~~Whether the operation carries the whole set or only the changed
  values.~~ **Answered, after the change was archived**: the whole set. A
  real operation from the tenant carried all three of a set's values with
  one of them changed. The client validates and applies whatever is
  present, and only writes what actually moved, so whole-set sends cost no
  extra flash. A value the operation omits keeps its current value.
- Whether a parameter should be able to be marked read-only (a value the
  device reports but the cloud cannot set), or whether that is just twin
  data.
