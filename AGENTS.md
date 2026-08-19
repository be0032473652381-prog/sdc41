# AGENTS.md — sdc41 project standing rules

Auto-loaded by Codex CLI on every session in this directory.
Debug/development firmware — no production restrictions.

---

## Scope

Standalone SCD41 parameter read-out harness on a dedicated RP2040 board,
separate from `luftfugl-motor` at build time.

## Hardware facts

- SCD41 on I²C0, GP4 (SDA) and GP5 (SCL).
- Console on UART1, pins as specified in `hardware.md`.

## Console

Full freedom — ANSI, fixed-screen, cursor positioning, whatever's useful.
Context for judgment calls: a prior project on similar UART1/GP-adjacent
wiring had its own escape sequences couple back into the receiver as
garbage input. If that happens here, it's a wiring/coupling issue on this
specific bench, not a reason to avoid the approach in general.

## I²C protocol facts

These describe what the SCD41 silicon requires, not project policy — they
don't change with how relaxed the project is:

- CRC-8 (poly 0x31, init 0xFF, no reflection) is required by the sensor on
  every two-byte **data** word, both directions. Command words carry no
  CRC. Get this wrong and the sensor NAKs or returns garbage — indistinguishable
  from a wiring fault unless checked directly against the datasheet's
  worked example, `CRC(0xBEEF) = 0x92`.
- The temperature offset register is an **unsigned** 16-bit field
  (`word = offset × 65536 / 175`). Negative values have no encoding —
  not a policy limit, just not representable in the bit pattern.

## Reporting

State assumptions and proceed rather than blocking on a clarifying question.
Say plainly when something can't be confirmed rather than filling in a
plausible-looking value.
