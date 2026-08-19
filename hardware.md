# hardware.md — sdc41 test harness configuration

What is physically connected. Config-only — behaviour is in `spec.md`.

---

## Board

| | |
|---|---|
| MCU board | RP2040-based development board — **model not yet specified** |
| Flash size | not yet confirmed — check board silkscreen/datasheet before setting `PICO_FLASH_SIZE_BYTES` |
| SWD adapter speed | 1000 kHz, drop to 100 kHz if the link is unreliable |

This is deliberately a different physical board from the one used in
`luftfugl-motor`. Do not assume flash size, silicon revision, or onboard
peripherals match that project.

---

## Pin connections

| Pin | Connected to | Peripheral | Direction |
|-----|--------------|-----------|-----------|
| GP4 | SCD41 SDA | I2C0 | bidir |
| GP5 | SCD41 SCL | I2C0 | bidir |
| GP8 | Debug probe UART RX | UART1 TX | out |
| GP9 | Debug probe UART TX | UART1 RX | in |

UART1 is placed on GP8/GP9 rather than GP0/GP1 or any pins adjacent to the
I²C bus, to avoid the TX/RX coupling problem encountered on a similarly
laid-out board in `luftfugl-motor`. Confirm GP8/GP9 are free on the actual
board in use before wiring — if they are already committed to something
else, choose a different UART1-capable pair, not GP0/GP1.

SWDIO/SWCLK are the board's dedicated debug pins, wherever they are broken
out on this specific board — not yet documented here since the board model
is unconfirmed.

---

## I²C bus

| Device | Address (7-bit) | Max clock |
|--------|-----------------|-----------|
| SCD41 | 0x62 | 100 kHz |

Single device on this bus. Pull-ups per the SCD41 datasheet's own
recommendation, 10 kΩ on SDA and SCL to 3.3 V — no other device sharing the
bus to justify a different value.

---

## SCD41

| | |
|---|---|
| Supply | 3.3 V, permanent — no power switching in this harness |
| VDD, VDDH | tied together |
| Power-up time | 1000 ms before commands accepted, after a hard reset |
| I²C address | 0x62 |
| CRC | mandatory on writes, poly 0x31, init 0xFF, no reflection |

Permanent power is a deliberate simplification for this test harness — the
goal is straightforward continuous read-out, not the power-cycled,
battery-optimised behaviour `luftfugl-motor` will eventually need. That
behaviour, when it's time to integrate, is a separate mode to add, not the
default here.

---

## Console — UART1

| | |
|---|---|
| Baud | 115200, 8N1 |
| Flow control | none |
| Ground | connect probe GND to board GND directly, not left floating |

---

## Open items

- Exact board model and its flash size — confirm before writing
  `PICO_FLASH_SIZE_BYTES` or any board-specific config.
- Confirm GP8/GP9 are genuinely free on the board in hand before wiring UART1
  there.
- SWD pin locations on this specific board — not yet documented.
