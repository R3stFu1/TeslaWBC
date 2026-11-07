# TeslaWBC

Local current control for a **Tesla Wall Connector Gen 3**, without the Tesla cloud.

An ESP32 reads a dedicated energy meter on the wallbox feed and poses as the Neurio meter that the Wall Connector polls over RS-485. By choosing which current it reports, it makes the charger hold a target current between 6 and 16 A (three-phase), or pause. The ESP32 has no logic of its own. The building platform (Aperta) sets the mode and the target over MQTT, and the ESP32 falls back to a configured target if Aperta goes silent.

## Hardware

| Part | Role |
|---|---|
| Olimex ESP32-POE-ISO-16MB-IND | Controller, powered and networked over PoE |
| 2 × Waveshare TTL TO RS485 (B) | One per bus, automatic direction control |
| Crompton DRS-100-3P MOD | Measures only the wallbox feed (Modbus RTU) |

### Pins

| Bus | Module pin | ESP32 pin | Olimex header |
|---|---|---|---|
| Meter (bus 1) | RXD | GPIO4 (TX) | UEXT pin 3 |
| Meter (bus 1) | TXD | GPIO36 (RX) | UEXT pin 4 |
| Charger (bus 2) | RXD | GPIO32 (TX) | GPIO header |
| Charger (bus 2) | TXD | GPIO33 (RX) | GPIO header |
| Both | VCC / GND | 3.3 V / GND | UEXT pins 1 and 2 |

The ESP32's TX goes to the module's RXD input. If a bus stays silent, swapping TX and RX at the module is the first thing to try.

Ethernet uses GPIO12, 17, 18, 19, 21, 22, 23, 25, 26 and 27. GPIO34 is the user button, GPIO35 and GPIO39 are battery and power sensing. The pins are set as substitutions at the top of `teslawbc.yaml`.

On the charger bus, connect A+ and B- to the Wall Connector's RS-485 terminals (red = A+, white = B-). The Wall Connector is terminated internally, so fit the 120 Ω terminator on the module at the cabinet end.

## How control works

The Wall Connector lowers the car's current whenever the meter reports more than its **Max Conductor Limit L**. TeslaWBC reports

```
R = max(W1, W2, W3) + (L - T)
```

on CT1 to CT3, where W is the wallbox's own phase current from the DRS-100 and T is the target. If the car draws more than T, R exceeds L and the charger turns it down. If it draws less, the charger lets it climb. The car settles at T after 5 to 20 s.

R follows the car's current 1:1 and is never below what the wallbox draws. Both matter: the charger stops trusting a meter that breaks these rules, sometimes for hours.

| Mode | Reported R | Charger behaviour |
|---|---|---|
| Stock | max(W) | Charges as if TeslaWBC were not there |
| Manual | max(W) + (L - T) | Holds T, clamped to 6 A up to Max Output Current. Below 6 A it pauses |
| Pause | max(L, max(W)) + 2 A | Opens the contactor, resumes when the mode changes |

### Fail-safes

- **Meter fault** (no valid reading for 3 s): report L + 2 A, so charging pauses. This applies in every mode, including Stock.
- **Aperta silent** (no command or heartbeat for longer than *Fallback Timeout*, default 5 min): Manual and Pause switch to *Fallback Target* (default 6 A; 0 means pause). Stock stays Stock.
- **ESP32 off or crashed**: the charger falls back to Tesla's built-in 6 A cap.
- **Stock mode** is the kill switch, without rewiring.

The logic is in [`teslawbc/control.h`](teslawbc/control.h), plain C++ with no ESPHome dependency, and is unit-tested in [`tests/`](tests/).

## MQTT interface (Aperta)

TeslaWBC talks to the building platform (Aperta) over MQTT. It connects to the broker set in `secrets.yaml` and uses the topic prefix `teslawbc`. All payloads are plain text: numbers as decimals, options as their names.

**Availability:** `teslawbc/status` is `online` while connected and `offline` (retained last will) when not.

### Commands (Aperta publishes)

| Topic | Payload | Notes |
|---|---|---|
| `teslawbc/select/mode/command` | `Stock`, `Manual` or `Pause` | Restored after a reboot |
| `teslawbc/number/target_current/command` | `0` to `32` (A) | Below 6 pauses. Not restored after a reboot: starts at 6 A until Aperta writes it |
| `teslawbc/heartbeat` | anything | Keeps the fallback from kicking in when nothing else changes |
| `teslawbc/number/max_conductor_limit/command` | A | Must match the Tesla app. Default 18 |
| `teslawbc/number/max_output_current/command` | A | Must match the Tesla app. Default 16 |
| `teslawbc/number/fallback_target/command` | A | Default 6; below 6 means pause |
| `teslawbc/number/fallback_timeout/command` | minutes | Default 5 |
| `teslawbc/button/restart/command` | `PRESS` | Reboots the ESP32 |

**Aperta must send a command or a heartbeat at least once per Fallback Timeout** (default 5 min), for example the target current every minute. If it stops, Manual and Pause switch to the Fallback Target. This also catches a stalled automation, not only a lost connection.

### States (TeslaWBC publishes, retained)

Each control above also reports its value on the same topic ending in `/state`, for example `teslawbc/select/mode/state`.

| Topic | Unit | Notes |
|---|---|---|
| `teslawbc/sensor/current_l1/state` (`_l2`, `_l3`) | A | Wallbox phase currents, 5 s average |
| `teslawbc/sensor/voltage_l1/state` (`_l2`, `_l3`) | V | 10 s average |
| `teslawbc/sensor/power_l1/state` (`_l2`, `_l3`) | W | 5 s average |
| `teslawbc/sensor/power/state` | W | Total, every 10 s |
| `teslawbc/sensor/frequency/state` | Hz | Every 10 s |
| `teslawbc/sensor/energy/state` | kWh | Import energy counter of the DRS-100 |
| `teslawbc/sensor/reported_current/state` | A | What the charger is told (R) |
| `teslawbc/sensor/effective_target/state` | A | The target in use; `nan` when not regulating |
| `teslawbc/text_sensor/state/state` | | `Stock`, `Charging`, `Paused`, `Fallback charging`, `Fallback paused`, `Meter fault` |
| `teslawbc/binary_sensor/meter_ok/state` | `ON`/`OFF` | |
| `teslawbc/binary_sensor/charger_polling/state` | `ON`/`OFF` | The Wall Connector is reading the emulated meter |
| `teslawbc/binary_sensor/fallback_active/state` | `ON`/`OFF` | |
| `teslawbc/sensor/charger_poll_interval/state` | ms | About 190 when polling |
| `teslawbc/sensor/uptime/state`, `teslawbc/text_sensor/firmware/state`, `teslawbc/text_sensor/ip_address/state` | | Diagnostics |

Solar surplus, schedules and tariffs are Aperta logic that sets Mode and Target Current.

Quick test from a PC:

```sh
mosquitto_sub -h <broker> -t 'teslawbc/#' -v
mosquitto_pub -h <broker> -t teslawbc/select/mode/command -m Manual
mosquitto_pub -h <broker> -t teslawbc/number/target_current/command -m 10
```

## Building and flashing

Requires ESPHome 2026.10 or newer.

```sh
cp secrets.yaml.example secrets.yaml   # then set the broker, login and a fresh OTA key
esphome run teslawbc.yaml              # first flash over micro-USB, later over the network
```

Flash over USB only while the board is **not** powered by PoE, or use a USB isolator.

Run the unit tests with `make -C tests`.

## Commissioning

1. Set the DRS-100 to 9600 baud, 8N1, address 1, and check that *Meter OK* is on and the currents look right.
2. In the Tesla app (More, "Tesla device settings"), add the energy meter: CT1 to CT3 = conductor, CT4 = none. Lower Max Output Current first, then set **Max Conductor Limit = 18 A**, and **Max Output Current = 16 A**. Enter the same values in TeslaWBC.
3. Check that *Charger Polling* is on and the poll interval is about 190 ms. If the charger does not poll, swap A and B, then try `charger_baud: "9600"`.
4. Leave it a day in **Stock** mode, then test Manual at 16, 10 and 6 A, then Pause and resume.
5. Block the Wall Connector's internet access at the router to freeze its firmware (tested on 26.34).

## Credits

The Neurio register map comes from the community reverse engineering in [LucaTNT's gist](https://gist.github.com/LucaTNT/4adf01a7252386559070023612efa117). The charger behaviour notes come from [PVi1/esphome-twc-control](https://github.com/PVi1/esphome-twc-control) and [zany92/tesla-loadpilot](https://github.com/zany92/tesla-loadpilot).

TeslaWBC is not affiliated with Tesla. Changing your wallbox installation is at your own risk, and wiring inside the Wall Connector is electrician work.
