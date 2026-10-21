# SMC-PAD hardware map

Board `SK12-V2.1 2024/04/29`. MCU **JieLi AC6966C** (AC696N/AC636N family, BR25, pi32v2, QFN32,
4 Mbit internal flash, 24 MHz crystal). Pin roles below are recovered from the stock firmware
`SMC-PAD_004` (`re/disasm/app004.dis`, VMA `0x1E00120`) unless marked *board*.

Confidence: **fw** = read directly from register/pin constants in the firmware;
**infer** = deduced from code behaviour, needs a continuity check.

## Pin map

| Pin | Role | Evidence | Conf. |
|---|---|---|---|
| PB6 | 74HC595 SRCLK (SPI2 CLK, map A) | SPI2 DMA in scan ISR `1e22fb6`; PB6 set output in init `1e0f254` | infer |
| PB7 | 74HC595 SER (SPI2 DO, map A) | same | infer |
| PB4 | 74HC595 RCLK (latch) | toggled around each SPI2 transfer in `1e22fb6` | fw |
| PB5 | 74HC595 /OE or 4051 INH | driven high early in init, low at the end (`1e0f16c`, `1e0f2d4`) | infer |
| PB0 | key-matrix sense row 0 (input, pull-up) | read as `PORTB.IN & 1` in `1e22fb6`; pin table entry 8 | fw |
| PC1 | key-matrix sense row 1 (input, pull-up) | read as `PORTC.IN & 2` in `1e22fb6`; pin table entry 7 | fw |
| PA5, PA2, PA0, PA1 | button-LED **segments** (sinks, active low) | pin table `0x1E1649F` = `05 02 00 01`, matching `PORTA.OUT \|= 0x27` in `1e23186` | fw |
| PA3, PC2, PC3 | button-LED **phases** (high-side commons, active low, one at a time) | `1e23186`: phase 0 cleared on `PORTA`, phases 1/2 on `PORTA + 0x80` = **PORTC** | fw |
| PB2, PB3, PC4 | HCF4051 select S0/S1/S2 (shared by both muxes) | set as outputs in ADC init `1e0f31a`; ISR `1e22d50` writes `idx & 7` | measured |
| PA4 | 4051 #1 COM → ADC ch3 (pads 0–7) | ADC `CH_SEL = 3 + (idx >= 8)` in `1e22d50`; PA4 set analog in init | fw |
| PA6 | 4051 #2 COM → ADC ch4 (pads 8–15) | same | fw |
| PB1 | pad RGB LED data (WS2812-style, SPI1 DO map A, DMA) | `1e11cba`/`1e10500`; driven by our console (`n`, `W`) | measured |
| PC5 | MIDI OUT TX (UART1 via output-channel mux) | `1e11dfa` PC5 mux config right before UART1 (`0x1E2100`) setup | infer |
| USBDP/USBDM | USB device (MIDI) | — | board |
| PD0–PD4 | internal SPI flash | standard JieLi | — |

All 22 user GPIOs of the QFN32 are accounted for (PA0–6, PB0–7, PC1–5).

## Button LEDs (ISR `1e23186`) — **the 595s are not involved**

A 3×4 multiplex of its own, recovered from the stock firmware and confirmed on the board
(driving PA2 or PA5 low lights LEDs):

- **3 phases** PA3 (phase 0), PC2 (phase 1), PC3 (phase 2): the ISR clears the current one and
  sets the other two, so they are **active-low high-side commons** — a PNP/P-FET feeds the rail.
  Phase 0 is written to `PORTA`, phases 1 and 2 to `PORTA + 0x80`. **That is PORTC**: the port
  register blocks are `0x40` apart (`map_adr(0x50, 0x10)` per port), so
  `PORTA 0x1E5000`, `PORTB 0x1E5040`, `PORTC 0x1E5080`. Reading `+0x80` as PORTB and driving
  PB2/PB3 lights nothing stable — the segment still sinks, so the LED briefly drains the local
  capacitance and fades out, which is exactly what the board did.
- **4 segments** PA5, PA2, PA0, PA1 (indices 0–3): set to **sink, active low**, when the LED is
  on, high when off. `PORTA.OUT |= 0x27` turns all four off at the top of each pass.
- A LED is lit when its phase is low **and** its segment is low, so showing more than one phase
  at a time needs the phases cycled (the stock ISR does one phase per call; ours multiplexes
  every 2 ms, see `btnled_multiplex()`).
- **LED map** (flash `0x1E1659A`, one byte per LED, `(phase << 4) | segment`):

  | LED | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
  |---|---|---|---|---|---|---|---|---|---|---|
  | byte | `10` | `03` | `12` | `01` | `11` | `22` | `21` | `20` | `02` | `00` |
  | phase | 1 | 0 | 1 | 0 | 1 | 2 | 2 | 2 | 0 | 0 |
  | segment | 0 | 3 | 2 | 1 | 1 | 2 | 1 | 0 | 2 | 0 |

  Two of the twelve positions (phase 1 segment 3, phase 2 segment 3) are unused.

- **Physical LEDs** — complete, identified on the board (all ten light cleanly, no ghosting):

  | idx | phase | seg | LED on the case | colour |
  |---|---|---|---|---|
  | 0 | 1 | 0 | left arrow (4th) | white |
  | 1 | 0 | 3 | right arrow (5th) | white |
  | 2 | 1 | 2 | play (6th) | green |
  | 3 | 0 | 1 | pause (7th) | red |
  | 4 | 1 | 1 | rec (8th) | red |
  | 5 | 2 | 2 | bluetooth (1st) | blue |
  | 6 | 2 | 1 | pad bank (2nd) | orange |
  | 7 | 2 | 0 | knob bank (3rd) | orange |
  | 8 | 0 | 2 | shift (9th) | white |
  | 9 | 0 | 0 | note repeat (10th) | white |

  Left to right on the case the order is therefore bluetooth, pad bank, knob bank, left, right,
  play, pause, rec, shift, note repeat — i.e. firmware indices 5, 6, 7, 0, 1, 2, 3, 4, 8, 9.

Runtime state lives at RAM `0x1A370`: `+12` current phase, `+106` three segment bytes (one per
phase), `+552` the ten on/off flags.

## Key matrix map (measured with the bring-up console's `M`)

Sense 1 (PC1) carries the **buttons**, sense 0 (PB0) the **encoders** — a clean split.

**Buttons — one per column on sense 1, columns 0–9** (column order is not panel order):

| col | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| button | shift | note repeat | bluetooth | pad bank | knob bank* | left arrow | right arrow | play | rec | pause |

\* `knob bank` is by elimination: it is the only free column, and its own reading was spoiled
by the interference below. Same for `right arrow` = c6, measured on the first clean pass.

Watch out: holding **phase 2 (PC3)** low drags the button sense (**PC1**, same port) low on
every column, so lighting a phase-2 LED while scanning reads "all columns at once" — that is
how bluetooth, pad bank and knob bank first failed. The console blinks the prompt LED and
samples only while it is off. **The real firmware has the same constraint: LED multiplexing
and matrix scanning must be interleaved in time, never overlapped.**

**Encoders — eight quadrature pairs on sense 0, adjacent columns**, filling all 16 positions.
They have no detent (they feel continuous), and the stock firmware decodes them the same way
in the ISR at `1e23076` (8 structs of 20 bytes). Measured with the console's `E` command,
by the number printed on the case:

| knob | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| pair | 7 | 5 | 0 | 4 | 3 | 6 | 2 | 1 |
| columns | c14/c15 | c10/c11 | c0/c1 | c8/c9 | c6/c7 | c12/c13 | c4/c5 | c2/c3 |

(pair N = columns 2N and 2N+1; all eight pairs are used, none twice.)

A quadrature contact is closed at rest in whichever position the knob is parked, so mapping
them needs changes measured in **both** directions; counting only contacts that close misses
half of them.

## Key scanning scheme (timer ISR `1e22fb6`)

- 16-step column scan: each step latches a 16-bit **one-hot-low** pattern
  (table `0x1E16914`: `FEFF FDFF FBFF F7FF EFFF DFFF BFFF 7FFF FFFE … FF7F`) into the two
  cascaded 74HC595 via SPI2 DMA, then samples the two sense rows (PB0, PC1).
- 16 columns × 2 rows = 32 switch positions with diodes for anti-ghosting: 8 encoders × 2 contacts
  + 10 buttons fit. Samples are stored as 16 × 2-bit entries.
- A second ISR (`1e23076`) decodes the 8 encoders from pairs of matrix bits (8 structs × 20 bytes,
  bit-pair table near `0x1E16490+572`).

## Pads (ADC ISR `1e22d50`) — mapped 2026-10-20

- Two HCF4051s share the select lines **S0 = PB2, S1 = PB3, S2 = PC4** (bit order confirmed: the
  stock table below turns our positions into a perfectly regular grid). COM of mux #1 → **PA4**
  (ADC channel 3), mux #2 → **PA6** (channel 4). PB5 low (as the stock init leaves it).
- ADC **position** = mux index for PA4 (0–7), 8 + mux index for PA6 (8–15).
- FSR to ground-referenced divider: **0 at rest** (silicone pads resting on top included), up to
  ~950–990 of 1023 pressed hard on the assembled pad.
- Stock firmware: 16 conversions per round from the ADC-done interrupt (next mux + start in the
  ISR), 16 thresholds of 150 (`1e0f31a`); per-pad calibration in flash `USRTRIM` (`0x7E000`,
  16-bit values ~`0x00DC…0x03B6`); two extra conversions after the pads (not identified).
- Stock tables (flash `0x17CF0`, `0x17D00` of the dump): **ADC position → stock pad**
  `2 0 1 3 15 14 12 13 6 4 5 7 8 9 11 10`, **stock pad → RGB chain index**
  `11 4 2 7 10 14 6 9 0 5 15 1 13 12 8 3`. Stock pad numbering: 0 = bottom-left, row by row
  upwards (MPC style).
- Measured with the console's `W` (pad 1 = top-left as you look at the pad):

  | | col 1 | col 2 | col 3 | col 4 |
  |---|---|---|---|---|
  | top row: pad / ADC pos / stock / LED | 1 / 6 / 12 / 13 | 2 / 7 / 13 / 12 | 3 / 5 / 14 / 8 | 4 / 4 / 15 / 3 |
  | row 2 | 5 / 12 / 8 / 0 | 6 / 13 / 9 / 5 | 7 / 15 / 10 / 15 | 8 / 14 / 11 / 1 |
  | row 3 | 9 / 9 / 4 / 10 | 10 / 10 / 5 / 14 | 11 / 8 / 6 / 6 | 12 / 11 / 7 / 9 |
  | bottom row | 13 / 1 / 0 / 11 | 14 / 2 / 1 / 4 | 15 / 0 / 2 / 2 | 16 / 3 / 3 / 7 |

## Pad RGB LEDs — mapped 2026-10-20

- WS2812-style chain on **PB1**, driven by **SPI1** (port A, DO = PB1; `IOMAP CON1` bit 4 = 0),
  `CON = 0x4021`, DMA (`ADR`, `CNT`), 6 MHz: the SPI clock is `clk / (2 × (BAUD + 1))`, so `BAUD = clk_get("spi") / 12 MHz - 1` (24 MHz → 1). With BAUD 3 (3 MHz) a 0 bit stays high 667 ns and reads as 1: every pad shows white on top of its colour (seen 2026-10-20; found by sweeping BAUD).
- One SPI byte per LED bit, MSB first: **`0xFC` = 1, `0xC0` = 0**; colour word **G<<16 | R<<8 | B**;
  frame = 2 zero bytes, 16 × 24 bytes, zeros up to 548 bytes (the reset gap), as the stock
  firmware sends it (`1e10500`).
- Chain index under each pad: the table above (confirmed by lighting it under the finger).

## Flash layout (from the 004 dump)

| Offset | Size | Content |
|---|---|---|
| `0x00000` | 4 KB | flash header + `uboot.boot` + `isd_config.ini` |
| `0x01000` | — | app area; `app.bin` at `0x1120` (VMA `0x1E00120`) |
| `0x26000` | 316 KB | VM (settings, magic `55 AA AA 55`) |
| `0x75000` | 4 KB | BTIF |
| `0x76000` | 4 KB | EXIF |
| `0x77000` | 28 KB | USRFLASH (user presets) |
| `0x7E000` | 4 KB | USRTRIM (pad calibration) |
| `0x7F000` | 4 KB | key_mac |

## To verify with the multimeter (board powered off)

1. PB6/PB7/PB4 → 595 #1 pins 11 (SRCLK) / 14 (SER) / 12 (RCLK); PB5 → pin 13 (/OE) or 4051 pin 6 (INH).
2. PB2/PB3/PC4 → 4051 pins 11/10/9 (S0/S1/S2) on both chips — gives the bit order.
3. PA4 / PA6 → 4051 pin 3 (COM) of each chip; which chip serves which pad row.
4. PB0 / PC1 → diode cathodes of the switch matrix.
5. PB1 → DIN of the first pad LED; PC5 → MIDI TRS (through its resistor/transistor).
6. The 7 row outputs → button-LED anodes.

The QFN32 pinout of the AC6966C is not in the reference docs here; trace from the package or
use the 595/4051 side as the known end.

## Power (2026-10-20)

- **Battery**: VBAT / 4 on the ADC's PMU mux (`AD_CH_VBAT`), converted with the bandgap
  reading (`AD_CH_LDOREF`, `adc_value_to_voltage`); sampled by our pad ADC chain every 64
  rounds (`smcpad_battery_mv()`). 4.1 V measured on USB.
- **USB 5 V is not visible to the chip**: `AD_CH_LDO5V` reads 0 and the SDK charge module is off
  (`TCFG_CHARGE_ENABLE` disabled): a separate charger does the battery (red / green LED in
  hardware). `smcpad_usb_power()` = a USB host enumerated us.

## Pad velocity calibration, case closed (2026-10-21)

105 hits over the 16 pads (`/tmp/PadCalib`: raw peak per hit): soft 215-770, hard 925-966,
very hard 976-988 (saturation ~990). All pads alike within that spread. The runtime maps the
peak linearly from the 150 threshold to **960 = velocity 127** (was 1023: 127 was never
reached and hard hits sat at ~115). Sketches use the same 150-960 range for pressure.
