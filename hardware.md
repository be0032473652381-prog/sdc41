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

YD-RP2040, RP2040-B2 silicon, 16 MB flash, confirmed by SFDP read once the
SWD link was stable. This board is wired independently of anything else —
no assumption about other boards' wiring applies here.

---

## Pin connections

| Pin | Connected to | Peripheral | Direction |
|-----|--------------|-----------|-----------|
| GP4 | SCD41 SDA | I2C0 | bidir |
| GP5 | SCD41 SCL | I2C0 | bidir |
| GP8 | Debug probe UART RX | UART1 TX | out |
| GP9 | Debug probe UART TX | UART1 RX | in |

GP8/GP9 confirmed free on this board. UART1 placed here rather than GP0/GP1
deliberately — adjacent TX/RX pins on this package are a known coupling
risk (transmitted output bleeding back into the receiver), and GP8/GP9 are
physically separated on the header.

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
| Supply | 3.3 V, permanent wiring — no physical switch on this board |
| VDD, VDDH | tied together |
| Power-up time | 1000 ms before commands accepted, after a hard reset |
| I²C address | 0x62 |
| CRC | mandatory on every two-byte data word, poly 0x31, init 0xFF, no reflection — **command words carry no CRC** |
| `power_down` | `0x36e0`, max 1 ms — sensor must be idle first |
| `wake_up` | `0x36f6`, max 20 ms — **the sensor never ACKs this command; do not treat a missing ACK as a write failure for this specific call** |

VDD is wired permanently — there is no MOSFET or load switch on this board.
Low-power state is controlled entirely at the protocol level, via the
sensor's own `power_down`/`wake_up` commands, driven from the console (see
`spec.md`). This is a different mechanism from physically cutting power —
the sensor stays powered throughout, just in a low-current internal state.

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
