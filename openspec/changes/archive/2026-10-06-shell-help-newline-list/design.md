## Context

`tedge_shell_help_text()` (`tedge-zephyr/src/tedge_shell_allow_list.c`)
already writes:

```
Commands this device runs (arguments may follow):
  kernel uptime
  net iface
  ...
```

The text travels through `tedge_shell_poll_event()` to `tedge_c8y.c`, which
calls `op_succeeded("c8y_Command", ev.output)`. That quotes the text with
`tedge_sr_quote()`, which replaces `\n` and `\r` with spaces because "a
newline would end the SmartREST line". The result is published as
`506,<opId>,"..."` (or `503,c8y_Command,"..."`) on `c8y/s/us`.

That comment is too cautious for a quoted field: SmartREST 2.0 parses CSV, and
a line break inside a double-quoted value is part of the value. thin-edge.io's
own c8y mapper reports multi-line c8y_Command output the same way.

`tedge_sr_quote()` has about ten other callers (remote access, log upload,
firmware, events, twin data) whose texts are meant to be one line.

## Goals / Non-Goals

**Goals:**
- `help` shows in Cumulocity as a header plus one command per line.
- Any c8y_Command output keeps its line structure.
- No new RAM, no change for other SmartREST messages.

**Non-Goals:**
- Larger results, a different `help` format, or changes to the allow-list.

## Decisions

1. **A separate quoting helper, used only for c8y_Command.**
   Add `tedge_sr_quote_lines(in, out, len)`, which behaves like
   `tedge_sr_quote()` but copies `\n` and drops `\r`. Both share one loop
   (static helper with a `keep_newlines` flag) so escaping and truncation stay
   identical.
   *Alternative*: change `tedge_sr_quote()` itself. Rejected: other callers
   build texts (reasons, event text, twin fragments) that should stay single
   line, and changing them all is outside this fix.

2. **Pass the choice through `op_succeeded`/`op_failed` only for
   c8y_Command.** Add `op_succeeded_lines()`/`op_failed_lines()` (or a
   `bool lines` parameter on a shared static) in `tedge_c8y.c`, and call them
   from the shell event loop. Failed results also keep lines, since a command
   that fails often prints a multi-line usage message.

3. **Drop `\r` rather than translate it.** Zephyr's dummy backend output ends
   lines with `\r\n`; keeping `\r` would render as stray characters or blank
   lines in the UI. A lone `\r` is rare enough that dropping it is fine.

4. **Keep the help text as it is.** The two-space indent under a header reads
   well once newlines survive, and the unit test already pins that format.
   Only the comment in `tedge_c8y.c` ("newlines become spaces") changes.

## Risks / Trade-offs

- [Cumulocity rejects or splits a quoted value with an embedded newline] →
  verify on hardware against the dev tenant before merging (task 3). If it is
  rejected, fall back to joining lines with a visible separator (e.g. `; `)
  inside `tedge_shell_help_text()` and leave the quoting alone.
- [Truncation cuts a line mid-way] → unchanged from today; truncation already
  happens at the 224-byte quoted buffer and still closes the quote.
- [A truncated value ends right after an escaped `"`] → the shared loop keeps
  the existing "need 2 bytes for `\"\"`" guard, so the field stays well formed.

## Migration Plan

None. Devices updated by OTA start sending multi-line results; older devices
keep sending single-line ones. Rollback is reverting the commit.

## Open Questions

- None blocking; the hardware check in task 3 settles the Cumulocity
  behaviour.
