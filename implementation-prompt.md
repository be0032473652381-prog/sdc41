# Task — Implement the sdc41 Test Harness

Build the complete firmware from the specification already in this
repository. Read `AGENTS.md`, `hardware.md` and `spec.md` in full before
writing any code — they are short and every constraint in them exists for a
reason stated inline.

Do not use `sed -i` by line number or `perl -0pi -e`. Use `apply_patch`.

---

## Scope

A CMake + Pico SDK project. Source in `src/`, standard layout. This is a new
repository with no existing source tree — create one.

Build both:

- `cmake -S . -B build`
- `cmake --build build -j4`

Producing `build/sdc41.elf`.

---

## What to implement

Everything in `spec.md`, in full:

- Boot sequence with the 100 ms progress reporting during the 1000 ms
  power-up wait, exactly as specified — ten lines, first at full 1000 ms
  remaining, last at 0.
- I²C driver for the SCD41 at address 0x62, with a correctly implemented
  CRC-8 (poly 0x31, init 0xFF, no reflection). Verify your implementation
  against the datasheet's own worked example, `CRC(0xBEEF) = 0x92`, before
  using it on any real command — report this check explicitly.
- Periodic measurement mode as the boot default, single-shot mode available
  via `mode single` / `mode periodic`.
- Every console command in `spec.md`'s table, each with the exact example
  given, each rejecting bad input with a plain-language reason.
- `help` and `help <command>`, covering every command that exists.
- Plain line-oriented console on UART1 (GP8 TX, GP9 RX), 115200 8N1, no
  stdio, no ANSI sequences of any kind — per `AGENTS.md`.

If any command's exact response format is not fully specified, choose a
format consistent with the style already shown in `spec.md`'s examples and
say what you chose.

---

## Do not add

No motor, no battery, no LED, no RTC, no sleep mode, no power switching on
the SCD41. `hardware.md` is explicit that supply is permanent. Do not add
any of this "for later."

---

## Build and flash — report these exact commands work

```
cmake -S . -B build
cmake --build build -j4
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 1000" -c "program build/sdc41.elf verify reset exit"
```

If OpenOCD or the interface/target config files need anything project-
specific (a `.cfg` file, a `boards/` entry for the YD-RP2040), add it and
say so — do not silently assume it already exists.

---

## Verification

Build must be clean with `-Wall -Wextra`, no warnings.

Report, from actual compiler/linker output:

- Final `build/sdc41.elf` size (text/data/bss)
- Confirmation the CRC-8 self-check against `0x92` passed
- Confirmation no ANSI escape sequences appear anywhere in the console
  output path (grep the source for escape sequences and report the result)

Do not flash. Do not connect to hardware. Report when the build is ready and
give the exact commands to build and flash — I will run them myself.

---

## Commit

Commit the new source tree with a clear message once it builds cleanly.
Do not modify `AGENTS.md`, `hardware.md`, or `spec.md` — if something in
them turns out to be wrong or incomplete once you're implementing, say so
and stop rather than editing the spec yourself.
