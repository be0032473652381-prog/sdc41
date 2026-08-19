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
| Temperature offset | `get_temperature_offset` | °C, affects RH/T only, not CO₂ |
| Sensor altitude | `get_sensor_altitude` | metres, for pressure compensation |
| Data-ready status | `get_data_ready_status` | whether a new reading is available |

**Ambient pressure has a set command in the SCD41 protocol but no
corresponding get command** — it cannot be read back once written. Do not
implement a `pressure` read command; note this limitation in `help` if the
command is referenced.

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
wait 1000 ms (SCD41 power-up requirement)
start periodic measurement
enter console loop
```

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
