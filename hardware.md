# hardware.md — sdc41 test harness configuration

What is physically connected. Config-only — behaviour is in `spec.md`.

---

## Board

| | |
|---|---|
| MCU board | YD-RP2040 |
| Silicon | RP2040-B2 |
| Flash | 16 MB, Zbit zb25vq128 |
| SWD adapter speed | 1000 kHz, drop to 100 kHz if the link is unreliable |

A separate physical unit from the board used in `luftfugl-motor`, same model.
Flash size and silicon revision confirmed — do not assume other onboard
peripherals or wiring match that unit; this board is wired independently.

---

## Pin connections

| Pin | Connected to | Peripheral | Direction |
|-----|--------------|-----------|-----------|
| GP4 | SCD41 SDA | I2C0 | bidir |
| GP5 | SCD41 SCL | I2C0 | bidir |
| GP8 | Debug probe UART RX | UART1 TX | out |
| GP9 | Debug probe UART TX | UART1 RX | in |

GP8/GP9 confirmed free on this board. UART1 placed here rather than GP0/GP1
to avoid the TX/RX coupling problem encountered on a similarly laid-out
board in `luftfugl-motor`.

### Debug — SWD

SWDIO and SWCLK are dedicated RP2040 package pins, broken out on the
YD-RP2040's 4-pin header marked `3V3 / GND / SWCLK / SWIO`. Connect SWCLK,
SWIO and GND to the Debug Probe. **Do not connect 3V3** — this board is
separately powered.

---

## I²C bus

| Device | Address (7-bit) | Max clock |
|--------|-----------------|-----------|
| SCD41 | 0x62 | 100 kHz |

Single device on this bus. Pull-ups per the SCD41 datasheet's own
recommendation, 10 kΩ on SDA and SCL to 3.3 V.

---

## SCD41

| | |
|---|---|
| Supply | 3.3 V, permanent — no power switching in this harness |
| VDD, VDDH | tied together |
| Power-up time | 1000 ms before commands accepted, after a hard reset |
| I²C address | 0x62 |
| CRC | mandatory on every two-byte data word, poly 0x31, init 0xFF, no reflection — **command words carry no CRC** |

Permanent power is a deliberate simplification for this test harness — the
goal is straightforward continuous read-out, not the power-cycled,
battery-optimised behaviour `luftfugl-motor` will eventually need.

---

## Console — UART1

| | |
|---|---|
| Baud | 115200, 8N1 |
| Flow control | none |
| Ground | connect probe GND to board GND directly, not left floating |

---

## Build and flash

```
cmake -S . -B build
cmake --build build -j4
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 1000" -c "program build/sdc41.elf verify reset exit"
```

## Open items

None outstanding. Board, pins and flash confirmed.
