## Context

`release/devices.yml` lists each device's builds as (app, variant). The
firmware name is `<app>-<variant>-<device>`, and it is everything a person
sees: the release files' stem, the firmware name a device reports, and its
firmware entry in Cumulocity. The operating system only shows where an app
directory (`tedge-agent-idf`) or a variant (`nuttx`) happens to name it.

The firmware name is at most 47 characters: tiny-edge keeps it in a 48-byte
buffer.

## Goals / Non-Goals

**Goals:**

- Every name says its app, operating system, feature set and board, in that
  order, so the same app's images on each operating system sort together.
- The manifest enforces the scheme, so a new build cannot break it.

**Non-goals:**

- Renaming app directories, device ids or device types.

## Decisions

### 1. `<app>-<os>-<set>-<board>`

The app comes first, so `tedge-agent-zephyr-full-…`, `tedge-agent-idf-full-…`
and `tedge-agent-nuttx-full-…` sit side by side. The set drops today's
`tedge-` prefix: `full` and `ota` only exist with thin-edge.io, and
`standalone` says it has none. With the prefix, the longest name would also
be at the 47-character limit, and so would the next.

Rejected: the operating system first (groups by operating system, but splits
one app's images apart, which is what a comparison needs side by side).

### 2. `idf` for ESP-IDF

ESP-IDF includes FreeRTOS. `idf` is short, and is what someone rebuilding
the image uses (`idf.py`). `freertos` would not say that the image is built
with ESP-IDF, and `esp-idf` would add a hyphen to a name that is split on
hyphens.

### 3. Variants are named `<os>-<set>`

The manifest's variants become `zephyr-standalone`, `zephyr-full`,
`zephyr-ota`, `idf-full` and `nuttx-full`, and the firmware name keeps its
formula, `<app name>-<variant>-<device>`. The operating system comes from
the variant's `framework` (`zephyr` → `zephyr`, `esp-idf` → `idf`, `nuttx`
→ `nuttx`), and the set from its new `set` field. `matrix.py` checks that
the variant's name is `<os>-<set>`, so the name cannot drift from how the
variant is built.

### 4. The app name can differ from the app directory

The ESP-IDF agent's project is `apps/tedge-agent-idf/`, beside the Zephyr
`apps/tedge-agent/`. A top-level `apps:` section in the manifest gives it
the name `tedge-agent`; an app without an entry is named after its
directory. `matrix.py` rejects two builds with the same firmware name.

### 5. One update moves a device to its new name

A device installs whatever firmware it is sent. It only compares names to
skip reinstalling the exact name and version it already runs. So a device
on `modbus-server-tedge-ota-esp32c6-devkitc 0.16.0` installs
`modbus-server-zephyr-ota-esp32c6-devkitc 0.17.0` from the new entry, and
from then on it reports the new name. The old entries in Cumulocity stay
until someone deletes them.

## Risks / Trade-offs

- A device left on an old name finds no newer version in its old entry.
  → The release notes say once that the names changed, and how to move.
- Links to old release files keep working, but the names differ from later
  releases. → DEVICES.md records each measurement under the name it had
  then, with a note at its top that maps old names to new ones.

## Migration Plan

1. Merge; the next release (0.17.0) has the new names.
2. Upload the release with `scripts/release/c8y-upload.sh`, which creates the
   new firmware entries.
3. Update each device once from its new entry.
