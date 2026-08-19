# sdc41

Standalone SCD41 CO₂/temperature/humidity sensor read-out harness, on its
own RP2040 development board. Independent of `luftfugl-motor` — built and
run entirely on its own — written for later integration into that project.

---

## Files

| File | Purpose |
|---|---|
| `AGENTS.md` | Standing rules, auto-loaded by Codex CLI |
| `hardware.md` | Pin connections, addresses, electrical facts |
| `spec.md` | Console commands and firmware behaviour |
| `README.md` | This file |

Read `AGENTS.md` and `hardware.md` before writing any code — several
constraints in them (UART1 pin choice, no ANSI console, CRC correctness)
exist because of specific problems hit on `luftfugl-motor` and are not
arbitrary.

---

## Status

Specification only. No source tree yet. Next step is an implementation
prompt for Codex CLI, scoped to `spec.md` and `hardware.md` as written.

---

## Quick start, once implemented

Then `help` for the full command list.

---

## Open items

None outstanding. Board, pins and flash confirmed.
