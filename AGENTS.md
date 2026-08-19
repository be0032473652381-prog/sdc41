# AGENTS.md — sdc41 project standing rules

Auto-loaded by Codex CLI on every session in this directory. These rules
take precedence over anything in a single prompt unless the prompt says
explicitly that it is a deliberate, authorised override.

---

## Scope

This project is a **standalone SCD41 parameter read-out harness** on a
dedicated RP2040 development board — separate hardware, separate repo, no
connection to the `luftfugl-motor` project at build time.

It is written with later integration into `luftfugl-motor` in mind, so
naming and console conventions deliberately follow that project's patterns.
Do not import or reference anything from `luftfugl-motor` directly — this
repo must build and run entirely on its own.

## Hardware constraints

- SCD41 on I²C0, GP4 (SDA) and GP5 (SCL) only. No other pins connected to
  the sensor.
- Console on UART1, pins as specified in `hardware.md`.
- No motor, no battery, no LED, no RTC, no sleep mode in this project. Do
  not add any of these speculatively "for later" — this repo's job is
  narrow: read the sensor, print everything it can report.

## Editing rules

- Use `apply_patch` with context. Do not use `sed -i` by line number or
  `perl -0pi -e` on source files.
- Read the current content of a file before editing it. Do not assume line
  numbers from a prior read remain valid after any edit.

## Console

- Plain, line-oriented text output only. **No ANSI escape sequences, no
  fixed-screen redraw, no cursor positioning.** A prior project on this
  UART1/GP-adjacency pattern lost significant time to a fixed-screen
  interface coupling its own escape sequences back into its receiver on a
  physically similar bench. A plain scrolling console has no such failure
  mode and is the deliberate choice here, not a placeholder to be replaced
  later.
- Echo typed characters as they arrive. Submit on Enter (CR or LF, not
  both).
- Every command that exists must appear in `help` with a working example.

## Build and flash

- CMake + Pico SDK, matching the standard `pico-sdk` project layout.
- `pico_enable_stdio_uart(<target> 0)` and `pico_enable_stdio_usb(<target> 0)`
  — raw UART driver, not SDK stdio, per this project's console requirement.
- Flash via OpenOCD, CMSIS-DAP, `adapter speed 1000` unless bench conditions
  require dropping to 100.

## I²C protocol correctness

The SCD41 requires a CRC-8 checksum (polynomial 0x31, initialisation 0xFF,
no input/output reflection) on every write, and provides one on every read
that the host may optionally verify. **This is the most error-prone part of
this project** — an incorrect CRC implementation typically fails silently
(the sensor NAKs or returns stale/garbage data) rather than with a clear
error, and looks identical to a wiring fault. Verify the CRC implementation
against the datasheet's own worked example (`CRC(0xBEEF) = 0x92`) before
trusting any sensor response.

## Reporting

When a task is ambiguous, state the assumption made and proceed — do not
block on a clarifying question if a reasonable default exists. When a
measurement or datasheet detail cannot be confirmed from the code or
hardware in hand, say so plainly rather than filling in a plausible-looking
value.
