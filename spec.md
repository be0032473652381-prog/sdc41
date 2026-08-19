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
| I²C address | — | fixed, compiled constant (0x62); not a sensor read — see below |

**Ambient pressure has a set command in the SCD41 protocol but no
corresponding get command** — a hardware limitation, not something a
`pressure` command could work around either way.

**Temperature offset field: unsigned 16-bit**, `word = offset × 65536 / 175`.
Negative values have no bit pattern to encode them into — reject negative
input, not because of a policy limit but because there's nothing to send.
Positive values up to the field's full width (~174.99 °C) are all
representable; no artificial ceiling below that.

**Sensor altitude field: unsigned 16-bit, 0 to 65535 m.** No range
restriction beyond what the field can hold — accuracy of the pressure
compensation at extreme values is a data-quality question, not a protocol
one, and this is a debug harness for exploring exactly that kind of thing.

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
| `menu` | `menu` | Print every readable parameter, one per line, key = value |
| `help` | `help` | List every command with an example |
| `help <cmd>` | `help offset` | Detail for one command |

Rejections must say what was wrong, in plain words — `rejected: offset must
be between -20 and 40 degrees`, not a bare error code.

---

## CO₂ filtering — median + EMA

The sensor already self-compensates every reading for temperature and
humidity on-chip via its own SHT4x — no host-side T/RH correction exists or
is added here. This section is about smoothing sample-to-sample noise, not
compensation.

Temperature and humidity display continues exactly as before, immediately,
unfiltered. Only `co2` is affected by anything in this section — that scope
is deliberate; nothing else was reported as unreliable.

### Sample collection — decoupled from display

A 7-sample median needs seven genuine sensor readings, roughly 30–35 s at
the SCD41's native periodic cadence (~5 s in high-performance mode; not
configurable down to 2 s in this mode). The filter runs on its **own poll
loop**, checking `data_ready` frequently (every ~1 s is enough to never
miss a reading), independent of the 5-second **display** refresh.

### The `co2` reading is withheld until the filter is fully warmed up

Unlike the earlier design, **no `co2` value is shown at all until the
7-sample ring buffer is completely full.** A partial-buffer median hasn't
had the chance to reject an outlier the way a true 7-sample median can, and
showing one anyway would undermine the entire point of adding the filter.
`co2` stays `pending` for the whole warm-up period, exactly as it already
does for the first few seconds before boot — just for longer, and with a
visible reason why.

### Warm-up progress field — `filter`, not `data ready`

A new field, distinctly named to avoid colliding with the existing
`data ready` (per-reading sensor flag, unaffected by any of this):

```
filter = 100%
filter = 86%
filter = 71%
filter = 57%
filter = 43%
filter = 29%
filter = 14%
filter = ready
```

Eight discrete states — 0 through 7 samples collected, `100 - (n/7)*100`,
rounded — counting down as each real sample arrives, styled the same as the
existing boot power-up countdown. **Updates only when a genuine new sample
arrives** (roughly every 5 s), not interpolated between polls — a debug tool
should show what's actually true, not a smoothed guess at it.

**This is a batch cycle, not a rolling window — it restarts every time.**
The moment a batch of 7 completes, `co2` updates to that batch's filtered
value and the buffer resets to empty. The very next raw sample belongs to a
fresh batch: `filter` immediately begins counting down from 100% again
(86% once that first new sample lands), and `co2` stays frozen at the value
from the previous completed batch for the entire ~35 s the next batch takes
to collect. `co2` therefore updates roughly once every 35 s, indefinitely,
never more often — this is deliberate: a slower, more clearly-batched
readout in exchange for every displayed value being a genuinely complete
7-sample filter result, not a partially-warmed one.

At the instant a batch completes, `filter = ready` and the new `co2` value
appear together — same event, so the operator can see the two are
connected — before the countdown restarts on the next sample.

### Pipeline, per completed batch of 7

1. Collect 7 raw `co2_ppm` readings into the buffer, one per real sensor
   sample (~5 s apart). `filter` counts down as each arrives.
2. Once the 7th arrives: **median of 7** — sort the batch, take the middle
   value.
3. **EMA across successive batch medians**, `alpha = 0.3`:
   `ema = alpha * batch_median + (1 - alpha) * ema_prev`. Seed `ema` with
   the first batch's median, not zero — the EMA smooths *between* batches,
   not within one.
4. Update `co2` to the new `ema` value. Reset the buffer to empty. The next
   raw sample starts the next batch immediately; `filter` returns to 100%.

### What the operator sees

`co2` shows the filtered (EMA-across-batches) value, updating roughly every
35 s. Also add `co2 raw` as its own field or command showing the most
recent unfiltered single reading, updating every ~5 s regardless of batch
state — never hide the raw signal entirely in a debug harness;
filtered-vs-raw comparison is how you confirm the filter is actually
helping.

### State

Ring buffer (7× `uint16_t`), fill count (0–7, reset to 0 every completed
batch), running EMA value (persists *across* batch resets — only the raw
sample buffer resets, not the EMA) — all in RAM, reset on boot.



Every parameter the SCD41 can report, in one plain-text dump. Runs once
automatically at the end of boot, again on every manual `menu` command, and
**auto-refreshes every 5 seconds while the console is idle**, matching the SCD41's own periodic measurement cadence so the refresh more often lands right after a fresh reading rather than between them.

### What refreshes every 5 seconds, and what does not

Two categories, deliberately treated differently:

| Field | Refreshed | Why |
|---|---|---|
| co2, temperature, humidity, mode | every 5 s | Available without leaving periodic measurement — no idle-mode round trip needed |
| serial, asc, offset, altitude | **once**, at boot — then cached | Reading these requires stopping periodic measurement, the datasheet's mandatory 500 ms silence, reading, then restarting. Doing that every 5 s would continuously interrupt periodic measurement. |
| i2c address | **never** — compiled constant | Not read from the sensor at all; it's the fixed address (0x62) this firmware is built to talk to. Printed once at boot, never changes for the life of the firmware. |

`data ready` is excluded from the auto-refresh — it flips true/false on its
own cadence independent of the other fields and was the dominant source of
scroll noise. It still appears in the boot-time and manual `menu` dumps.

The cached config fields are re-read and updated only when:

- the operator runs `menu` explicitly, or
- the operator changes one via `asc on/off`, `offset <°C>`, or
  `altitude <m>` — that command's own set path already talks to the sensor,
  so update the cached value from its result rather than triggering a
  separate read.

### Idle-only refresh, not mid-typing

The 5-second auto-refresh **only fires when the console is at an empty
prompt** — no partial command line pending. If the operator is mid-way
through typing a command, suspend the refresh; resume the 5-second countdown
from the next completed command or from the last keystroke, whichever is
more recent. A refresh block appearing in the middle of a half-typed command
would be confusing on a plain scrolling console with no redraw.

This auto-refresh runs for the life of the session, not just a few cycles
after boot — there is no separate toggle to turn it on or off in this
version. (Open item: if this proves too noisy in practice, a `menu off` /
`menu auto` toggle is a natural follow-up, not implemented here.)

### Auto-refresh prints only changed fields

The boot-time menu and any explicit `menu` command still print all nine
fields, always — that is the operator's "show me everything now" path and
must remain a complete, reliable dump.

The **automatic** 5-second refresh is different: print a field's line only
if its value has changed since the last time that field was printed
(whether at boot, on an explicit `menu`, or on a previous auto-refresh).
Unchanged fields print nothing that cycle.

- If nothing at all changed since the last print, the cycle produces **no
  output whatsoever** — not even a blank line. Silence is the expected,
  correct result of no change.
- Compare on the underlying value (the integer milli-degree, milli-percent,
  ppm, or raw count), not on the formatted string — avoids any risk of a
  rounding artifact in formatting being mistaken for a real change.
- `co2`, `temperature`, `humidity` and `data ready` come from the same
  measurement read and will typically change together, so they will usually
  appear as a small cluster of lines each real refresh — this is expected
  and does not need any separating marker between cycles.
- No timestamps or delimiters are added between refresh cycles. Keep this
  change scoped to suppressing unchanged lines; nothing else about the
  format changes.

Still no ANSI escape sequences, no cursor positioning, no fixed-screen
redraw — this stays a plain scrolling log, per `AGENTS.md`, with fewer
repeated lines rather than a different display mechanism.

```
co2 = 612 ppm
temperature = 23.4 C
humidity = 41.2 %RH
serial = 273325796834238
asc = on
offset = 4.500 C
altitude = 0 m
mode = periodic
data ready = yes
```

Format: `key = value`, one per line, CRLF, no alignment padding required.
Units included on the same line as the value, not in a separate column.

**`selftest` is deliberately excluded** from `menu` — it blocks for ~10 s and
interrupts measurement. Running it on every boot or every manual `menu` call
would make the display unusable to watch. It remains available only as its
own explicit command.

**Immediately after boot, CO₂/temperature/humidity may not be ready yet** —
periodic measurement has only just started and the first reading can take
up to 5 s. Print `pending` for those three fields rather than blocking boot
to wait for them:

```
co2 = pending
temperature = pending
humidity = pending
serial = 273325796834238
...
```

The other fields (serial, ASC, offset, altitude, mode) are always available
immediately and print real values even on the very first boot call.

---

## Boot sequence

```
power on
wait 1000 ms (SCD41 power-up requirement), printing progress every 100 ms
start periodic measurement
print the menu display, once (see "The menu display" above)
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
