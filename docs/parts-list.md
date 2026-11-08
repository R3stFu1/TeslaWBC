# TeslaWBC: parts list for both boxes

Status as of 8 Nov 2026. "Have" = already bought or on hand; "order" = still to buy.

## Enclosures
| Qty | Part | Status |
|---|---|---|
| 2 | Legrand Plexo3 Feuchtraumverteiler, IP65, 1 row × 12 modules | have |

## Box 1: low voltage
| Qty | Part | Status |
|---|---|---|
| 1 | Olimex ESP32-POE-ISO-16MB-IND | have |
| 1 | Olimex DIN-CLIP-ESP32-POE-ISO (DIN-rail clip) | ordered |
| 2 | Waveshare TTL TO RS485 (B), rail-mount | have |
| 1 | Driver board parts (BC337-40, 1 kΩ, 330 Ω, 1N4148, AKL terminals, perfboard 72 × 50 mm, 2-pin header for J3) | Reichelt cart |
| 1 | Delock 65989 PCB holder, 5 cm | Reichelt cart |
| 2 | Pin header 1×10 (EXT1, EXT2) | Reichelt cart |
| 3 | Dupont jumper wires female-female | have |
| 7 | Phoenix PT 2,5 grey (X1) | terminal order |
| 1 | Phoenix D-ST 2,5 end plate | terminal order |
| 2 | Wago 249-116 end stop, 6 mm | ordered |

## Box 2: high voltage (electrician)
| Qty | Part | Status |
|---|---|---|
| 1 | Crompton DRS-100-3P MOD meter | have |
| 1 | ABB ESB25-04N-06 contactor (230 V coil, 4 NC) | ordered |
| 1 | ABB S201-C2 breaker (-F2) | Reichelt cart |
| 1 | Finder 38.51.7.005.0050 (5 V coil) | ordered |
| 4 | Phoenix PT 2,5 grey (X2) | terminal order |
| 6 | Phoenix PT 2,5 grey (X3 L1–L3 in/out) | terminal order |
| 2 | Phoenix PT 2,5 BU blue (X3 N in/out) | terminal order |
| 1 | Phoenix PT 2,5-PE green-yellow (X3 PE) | terminal order |
| 2 | Phoenix D-ST 2,5 end plate (X2, X3) | terminal order |
| 1 | Wago 249-116 end stop, 6 mm | ordered |

Terminal order in total: 19 × PT 2,5 grey (incl. 2 spares), 2 × PT 2,5 BU, 1 × PT 2,5-PE (+1 spare), 3 × D-ST 2,5. End stops: 3 × Wago 249-116 (2 for Box 1, 1 for Box 2).
The X3 power terminals need a 2.5 mm² feed cable. If the feed is thicker, wire it straight to the meter and the contactor instead.

## Installation material
| Qty | Part | Status |
|---|---|---|
| ~6 | Cable glands (Kabelverschraubungen) matching the Plexo3 entries: Box 1 LAN, Cable A, Cable B; Box 2 Cable A, supply, Wall Connector | ordered |
| – | Wire-end ferrules (Aderendhülsen) 0.25–2.5 mm² + crimp tool | **order** |
| – | Stranded wire 0.25–0.5 mm² for Box 1 | check |
| – | Cable A: J-Y(St)Y 2×2×0.8 (Box 1 ↔ Box 2) | have |
| – | Cable B: J-Y(St)Y 2×2×0.8 or CAT 6 (Box 1 ↔ Wall Connector) | have |
| – | LAN cable PoE switch ↔ Box 1 | check |
| – | Power wiring in Box 2, cable to the Wall Connector | electrician |

Drawings: [wiring-diagram.png](wiring-diagram.png), [cabinet-layout.png](cabinet-layout.png).
