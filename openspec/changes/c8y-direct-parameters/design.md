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

`c8y_ParameterUpdate` carries the values to change. The client checks every
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

### D6: The operation is answered like every other one

`c8y_ParameterUpdate` arrives as JSON with its id (`c8y-operation-ids`), is
turned into a line the dispatcher understands, and is completed with
`506,<id>` or failed with `505,<id>,<reason>`. It runs on the client thread:
validating and storing a handful of values is microseconds, and there is
nothing to wait for.

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

- What exactly `c8y_ParameterUpdate` carries for a set — the whole set or
  only the changed values — and whether the tenant's template 532 shape
  matches what the plugin documents. To be settled against the tenant first.
- Whether a parameter should be able to be marked read-only (a value the
  device reports but the cloud cannot set), or whether that is just twin
  data.
