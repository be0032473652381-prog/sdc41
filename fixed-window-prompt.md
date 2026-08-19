# Task — Fixed-Position Display, Replacing the Scrolling Menu

AGENTS.md now permits ANSI/fixed-screen display for this debug build. Build
one: values update in place at fixed screen positions, nothing scrolls.

Do not use `sed -i` by line number or `perl -0pi -e`. Use `apply_patch`.

---

## Layout

Nine fixed rows, drawn once on entry, then only individual values rewritten
in place afterward:

```
luftfugl sdc41 — SCD41 test harness

  co2         = 1034 ppm
  temperature = 24.540 C
  humidity    = 60.309 %RH
  serial      = 74947532110605
  asc         = on
  offset      = 4.000 C
  altitude    = 0 m
  mode        = periodic
  data ready  = yes

> _
```

Row 1: title. Row 2: blank. Rows 3–11: the nine fields, label left-aligned in
a fixed-width column so the `=` lines up down the screen. Row 12: blank.
Row 13: command prompt, `> ` followed by whatever the operator is typing.
Results of commands (`help`, `selftest`, rejection messages, etc.) go below
the prompt in a small scrolling region — only the nine status fields are
fixed, not command output.

## Drawing

- On entry: `ESC[2J` (clear screen), `ESC[H` (home), draw every row once.
- To update one field: move the cursor to that field's exact row/column with
  `ESC[<row>;<col>H`, write the new value padded to a fixed width (so a
  shorter new value doesn't leave stale characters from a longer old one —
  use `ESC[K` to clear to end of line, or pad with spaces), then return the
  cursor to the command line position so typing isn't disrupted.
- Never redraw the whole screen to update one value. Touch only the field
  that changed.
- The command line (row 13) and anything the operator is actively typing
  must never be overwritten by a background field update. Save cursor
  position before a field update (`ESC[s`), do the update, restore it
  (`ESC[u`) — or explicitly reposition to the command line's known row/column
  afterward.

## Reuse existing logic

The auto-refresh timing (every 3 s, idle-only, pauses while typing), the
change-suppression (skip a field if its value hasn't changed), and the
cached-vs-live field split (serial/asc/offset/altitude cached at boot;
co2/temperature/humidity/mode/data-ready live) all stay exactly as already
implemented. Only the *output mechanism* changes — from
`console_write("field = value\r\n")` to "move cursor to field's row, write
padded value." Do not change the timing, caching, or suppression logic.

## Known risk on this bench, watch for it

AGENTS.md documents that a prior project on similar UART1/GP-adjacent wiring
had escape sequences couple back into the receiver as input. After
implementing, test specifically: type a command while several field updates
are firing in the background, and confirm the typed characters land
correctly on the command line and are not corrupted or interleaved with
escape codes. If input breaks, report it exactly rather than working around
it silently — that would tell us whether this bench has the same coupling
issue, which is useful information either way.

## Verification

Report actual console output (as raw bytes/escape sequences, not just
rendered text) for:

1. Initial draw on boot
2. One field (co2) updating in place — confirm no scrolling, no reprint of
   other rows
3. Typing `help` character by character while a background refresh fires —
   confirm the command line isn't corrupted
4. `menu` command still produces a full redraw of all nine fields correctly

Do not flash or load — I do that myself via SWD.
