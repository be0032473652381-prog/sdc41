# spec.md — sdc41 test harness behaviour

What the firmware does. Pins and electrical facts are in `hardware.md`.

---

## Purpose

Read out every parameter the SCD41 can report, over a plain console on
UART1, for bench characterisation ahead of integrating this sensor into
`luftfugl-motor`.

---

## Measurement mode

Default: **high-performance periodic measurement**, 5-second update
interval. This is the simplest way to see continuously live values while
characterising the sensor, and is not battery-optimised — that is
deliberate, since this harness is powered permanently (`hardware.md`).

`mode single` switches to single-shot mode, for later testing of the
power-cycled behaviour `luftfugl-motor` will eventually use. `mode periodic`
switches back. Boot default is periodic.

---

## Every parameter the SCD41 can report

This harness must expose all of the following, not just CO₂:

| Parameter | Source | Notes |
|---|---|---|
| CO₂ concentration | periodic/single-shot measurement | ppm |
| Temperature | periodic/single-shot measurement | °C, from the built-in SHT4x |
| Relative humidity | periodic/single-shot measurement | %RH, from the built-in SHT4x |
| Serial number | `get_serial_number` | 48-bit, one-off identity read |
| Self-test result | `perform_self_test` | pass/fail, takes up to 10 s |
| ASC enabled state | `get_automatic_self_calibration_enabled` | on/off |
| Temperature offset | `get_temperature_offset` | °C, affects RH/T only, not CO₂ — see range note below |
| Sensor altitude | `get_sensor_altitude` | metres, for pressure compensation — see range note below |
| Data-ready status | `get_data_ready_status` | whether a new reading is available |

**Ambient pressure has a set command in the SCD41 protocol but no
corresponding get command** — it cannot be read back once written. Do not
implement a `pressure` read command; note this limitation in `help` if the
command is referenced.

**Temperature offset range: 0 to 20 °C.** The protocol encodes this field as
`word = offset × 65536 / 175` on an **unsigned** 16-bit value — a negative
offset has no valid encoding and must be rejected, not clamped or silently
made positive. 20 °C is the practical upper bound; the field can technically
carry a much larger raw value, but do not accept anything the datasheet does
not describe as a sensible offset. Reject outside 0–20 with a plain-language
reason.

**Sensor altitude range: 0 to 3000 m.** Reject outside this range with a
plain-language reason.

*(These two range figures come from Codex's own reading of the datasheet
during implementation, cross-checked here only for the encoding constraint —
the 0 lower bound on offset — which is independently verifiable from the
formula above. If either upper bound turns out wrong once the datasheet is
checked directly, correct it here rather than silently overriding it in
code.)*

---

## Console commands

Plain text, one line per command, per `AGENTS.md`. Every command example
must be typeable exactly as shown.

| Command | Example | Effect |
|---|---|---|
| `co2` | `co2` | Print the latest CO₂, temperature and humidity |
| `ready` | `ready` | Print data-ready status |
| `serial` | `serial` | Print the 48-bit serial number |
| `selftest` | `selftest` | Run self-test, report pass/fail (blocks ~10 s) |
| `asc` | `asc` | Print current ASC state |
| `asc on` / `asc off` | `asc off` | Set ASC state |
| `offset` | `offset` | Print current temperature offset |
| `offset <°C>` | `offset 4.5` | Set temperature offset |
| `altitude` | `altitude` | Print current sensor altitude |
| `altitude <m>` | `altitude 12` | Set sensor altitude |
| `mode` | `mode` | Print current measurement mode |
| `mode periodic` / `mode single` | `mode single` | Switch measurement mode |
| `status` | `status` | Mode, ASC state, last reading, data-ready state, in one summary |
| `help` | `help` | List every command with an example |
| `help <cmd>` | `help offset` | Detail for one command |

Rejections must say what was wrong, in plain words — `rejected: offset must
be between -20 and 40 degrees`, not a bare error code.

---

## Boot sequence

```
power on
wait 1000 ms (SCD41 power-up requirement), printing progress every 100 ms
start periodic measurement
enter console loop
```

While waiting, print a progress line every 100 ms, **inclusive of both
ends** — the first line at the very start of the wait, the last at the
moment it completes:

```
booting: SCD41 power-up, 1000 ms remaining
booting: SCD41 power-up, 900 ms remaining
booting: SCD41 power-up, 800 ms remaining
...
booting: SCD41 power-up, 100 ms remaining
booting: SCD41 power-up, 0 ms remaining
```

That is **eleven lines** in total — 1000 down to 0 in steps of 100,
including both endpoints. The first line prints immediately at boot, before
any time has elapsed. The last line prints the moment the wait completes,
then periodic measurement starts.

No console command may be issued before the 1000 ms power-up window has
passed — if one arrives early, queue it or reject it with a clear message
rather than sending it to a sensor that is not yet ready.

---

## Verification, once implemented

1. `co2` — a plausible reading, updating every 5 s in periodic mode
2. `serial` — a real, non-zero 48-bit value
3. `selftest` — completes and reports its result
4. `asc off` then `asc` — confirms the change took
5. `offset 4.5` then `offset` — confirms the change took
6. `mode single` then `co2` — confirms single-shot mode works, including its
   own timing requirements
7. A deliberately wrong CRC on a test write (if feasible to simulate) —
   confirms the implementation is checked, not just present

Report actual console output for each, not a summary.
