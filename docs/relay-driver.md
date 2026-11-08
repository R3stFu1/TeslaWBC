# Relay driver board (piggyback for the recovery relay)

GPIO13 of the ESP32 can source only about 20 mA at 3.3 V. The Finder 38.51 interface relay with a 5 V coil needs about 35 to 40 mA at 5 V. This small board sits between them: GPIO13 switches a transistor, and the transistor switches the relay coil from the board's 5 V.

Only needed for the optional automatic recovery (see README, *Automatic recovery*).

## Bill of materials

| Ref | Qty | Part | Value / type | Notes |
|---|---|---|---|---|
| Q1 | 1 | NPN transistor | BC337-40, TO-92 | 800 mA, hFE ≥ 250; BC337-25 or 2N2222A also work |
| R1 | 1 | Resistor | 1 kΩ, 0.25 W | Base resistor, about 2.6 mA from GPIO13 |
| R2 | 1 | Resistor | 10 kΩ, 0.25 W | Base pull-down: keeps the relay off while the ESP32 boots or if the wire comes loose |
| D1 | 1 | Diode | 1N4148 (or 1N4007) | Flyback diode across the coil. The Finder module has its own protection; D1 is a cheap second line |
| J1 | 1 | Screw terminal | 3-pole, 5.08 mm | To the ESP32: +5V, GND, GPIO13 |
| J2 | 1 | Screw terminal | 2-pole, 5.08 mm | To the Finder coil: A1 (+), A2 (−) |
| — | 1 | Stripboard or perfboard | about 30 × 40 mm | |
| — | 1 | Jumper cable | 3 wires, female 2.54 mm on the ESP32 end | +5V, GND, GPIO13 |
| — | 1 | Housing or DIN-rail PCB carrier | fits 30 × 40 mm | Keeps the board closed in the panel |
| — | 1 | Pin header strip | 2.54 mm | Only if the Olimex EXT headers are not soldered yet |

Elsewhere in the panel (already in the main parts list): Finder 38.51.7.005.0050 (5 V DC coil), ABB ESB25-04N-06 contactor, a 2 A fuse for the coil circuit.

## Circuit

```
 ESP32-POE-ISO                 driver board                       Finder 38.51 (5 V)
                         J1                       J2
 +5V (EXT1 pin 1) ──────── 1 ──────────┬────────── A1 (+) ─┐
                                       │                     coil
                                  D1 ─┴─ (cathode to +5V)    │
                                  1N4148                      │
                                       ├────────── A2 (−) ─┘
                                       │
                                       C
 GPIO13 ───────────────── 3 ── R1 1k ── B   Q1 BC337
                                │      E
                              R2 10k   │
                                │      │
 GND ──────────────────── 2 ───┴──────┴──── GND
```

- GPIO13 high → Q1 conducts → coil on → Finder contact closes → 230 V on the ABB coil → Wall Connector **off**.
- GPIO13 low, floating or ESP32 off → R2 holds Q1 off → Finder open → ABB closed → Wall Connector **on**.

## Wiring to the Olimex board

| Driver J1 | Olimex ESP32-POE-ISO |
|---|---|
| 1: +5V | EXT1 pin 1 (+5V). Fed by PoE; the ISO board can supply about 300 mA to own circuits |
| 2: GND | any GND pin |
| 3: GPIO13 | the pin marked GPIO13 on the EXT header (check the silkscreen). GPIO13 is also UEXT pin 6; use only one of the two |

The meter's RS-485 module uses UEXT pins 1 to 4 only, so GPIO13 is free.

## Checks before connecting the Finder

1. Power the ESP32 over PoE. Measure J2: 0 V between A1 and A2.
2. Press *Recover Wall Connector* on the web page with the Wall Connector's breaker **off**. J2 shows about 5 V for 1 minute; the Finder LED lights.
3. *Cancel Recovery*: back to 0 V.
4. Only then connect the Finder and, by an electrician, the 230 V side to the ABB coil.
