# TeslaWBC

Local current control for a **Tesla Wall Connector Gen 3**, without the Tesla cloud.

An ESP32 reads a dedicated energy meter on the wallbox feed and poses as the Neurio meter that the Wall Connector polls over RS-485. By choosing which current it reports, it makes the charger hold a target current between 6 and 16 A (three-phase), or pause. The ESP32 has no logic of its own. The building platform (Aperta) sets the mode and the target over MQTT or REST, and the ESP32 falls back to a configured target if Aperta goes silent.

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
| Bypass | max(W) | Charges as if TeslaWBC were not there |
| Manual | max(W) + (L - T) | Holds T, clamped to Min Current (default 6 A) up to Max Output Current. Below Min Current it pauses |
| Pause | max(L, max(W)) + 2 A | Opens the contactor, resumes when the mode changes |

### Fail-safes

- **Meter fault** (no valid reading for 3 s): report L + 2 A, so charging pauses. This applies in every mode, including Bypass.
- **Aperta silent** (no command or heartbeat for longer than *Fallback Timeout*, default 5 min): Manual and Pause switch to *Fallback Target* (default 6 A; 0 means pause). Bypass stays Bypass.
- **ESP32 off or crashed**: the charger falls back to Tesla's built-in 6 A cap.
- **Bypass mode** is the kill switch, without rewiring.
- **Control Lost alarm**: if the charger ever judges the meter implausible, it ignores it, sometimes for hours, and charges at its own maximum; Pause stops working too. TeslaWBC raises *Control Lost* when the car draws more than 1.5 A above the target, or more than 1 A during a pause, for 3 minutes. It clears as soon as the charger follows again. Aperta should alert on it. Recovery that worked for others: switch the Wall Connector off and on, then leave TeslaWBC in Bypass for a few hours.

### Wall Connector status

TeslaWBC also reads the Wall Connector's local API (`/api/1/vitals`) every 10 s and passes on vehicle connected, contactor closed, vehicle current, session energy, session time and EVSE state. This is for information only and never used for control. Set the address in the `twc_vitals_url` substitution and give the Wall Connector a fixed IP in your router. If it is unreachable, *WC Reachable* goes off and control carries on unaffected.

The logic is in [`teslawbc/control.h`](teslawbc/control.h), plain C++ with no ESPHome dependency, and is unit-tested in [`tests/`](tests/).

## Interface to Aperta: MQTT and REST

TeslaWBC offers the same functions over two interfaces, and Aperta can use either or both.

- **MQTT:** connects to the broker in `secrets.yaml` with the topic prefix `teslawbc`. TeslaWBC publishes every change as it happens, retained. Aperta sends commands to `…/command` topics. Payloads are plain text.
- **REST:** HTTP on port 80 of the ESP32, protected with digest authentication (login in `secrets.yaml`). URLs use the entity name, URL-encoded (`%20` for a space). `GET` returns JSON such as `{"id":"sensor/Current L1","value":15.92,"state":"15.92 A"}`. `POST` returns 200 on success. A browser on `http://<ip>/` shows a status page with all values and controls.

| Function | MQTT | REST |
|---|---|---|
| **Set mode** | publish `Bypass`, `Manual` or `Pause` to `teslawbc/select/mode/command` | `POST /select/Mode/set?option=Manual` |
| **Set target current** (0 to 32 A; below Min Current pauses) | publish `10` to `teslawbc/number/target_current/command` | `POST /number/Target%20Current/set?value=10` |
| **Heartbeat** (keeps the fallback away) | publish `PRESS` to `teslawbc/button/heartbeat/command` | `POST /button/Heartbeat/press` |
| Read mode and target | `teslawbc/select/mode/state`, `teslawbc/number/target_current/state` | `GET /select/Mode`, `GET /number/Target%20Current` |
| **Controller state** (`Bypass`, `Charging`, `Paused`, `Fallback charging`, `Fallback paused`, `Meter fault`) | `teslawbc/text_sensor/state/state` | `GET /text_sensor/State` |
| Current per phase (A, 5 s average) | `teslawbc/sensor/current_l1/state` (`_l2`, `_l3`) | `GET /sensor/Current%20L1` (`L2`, `L3`) |
| Voltage per phase (V, 10 s average) | `teslawbc/sensor/voltage_l1/state` (`_l2`, `_l3`) | `GET /sensor/Voltage%20L1` (`L2`, `L3`) |
| Power per phase (W, 5 s average) | `teslawbc/sensor/power_l1/state` (`_l2`, `_l3`) | `GET /sensor/Power%20L1` (`L2`, `L3`) |
| Total power (W), frequency (Hz) | `teslawbc/sensor/power/state`, `teslawbc/sensor/frequency/state` | `GET /sensor/Power`, `GET /sensor/Frequency` |
| Energy counter (kWh) | `teslawbc/sensor/energy/state` | `GET /sensor/Energy` |
| Current reported to the charger (R) | `teslawbc/sensor/reported_current/state` | `GET /sensor/Reported%20Current` |
| Target in use (A, `nan` when not regulating) | `teslawbc/sensor/effective_target/state` | `GET /sensor/Effective%20Target` |
| Health flags (`ON`/`OFF`) | `teslawbc/binary_sensor/meter_ok/state`, `…/charger_polling/state`, `…/fallback_active/state` | `GET /binary_sensor/Meter%20OK`, `/binary_sensor/Charger%20Polling`, `/binary_sensor/Fallback%20Active` |
| **Control Lost** (`ON` when the charger stops following TeslaWBC, see below) | `teslawbc/binary_sensor/control_lost/state` | `GET /binary_sensor/Control%20Lost` |
| Wall Connector: vehicle connected, contactor closed, reachable (`ON`/`OFF`) | `teslawbc/binary_sensor/wc_vehicle_connected/state`, `…/wc_contactor_closed/state`, `…/wc_reachable/state` | `GET /binary_sensor/WC%20Vehicle%20Connected`, `/binary_sensor/WC%20Contactor%20Closed`, `/binary_sensor/WC%20Reachable` |
| Wall Connector: vehicle current (A), session energy (kWh), session time (s), EVSE state | `teslawbc/sensor/wc_vehicle_current/state`, `…/wc_session_energy/state`, `…/wc_session_time/state`, `…/wc_evse_state/state` | `GET /sensor/WC%20Vehicle%20Current`, `/sensor/WC%20Session%20Energy`, `/sensor/WC%20Session%20Time`, `/sensor/WC%20EVSE%20State` |
| Charger poll interval (ms, about 190) | `teslawbc/sensor/charger_poll_interval/state` | `GET /sensor/Charger%20Poll%20Interval` |
| **Device online?** | `teslawbc/status` = `online`, or `offline` via the broker's last will | no answer to a request (Aperta polls) |
| **Live updates** | subscribe to `teslawbc/#` | `GET /events` (Server-Sent Events stream) |
| Settings: Max Conductor Limit, Max Output Current, Min Current, Fallback Target, Fallback Timeout | `teslawbc/number/max_conductor_limit/command` (and `max_output_current`, `min_current`, `fallback_target`, `fallback_timeout`), values on `…/state` | `POST /number/Max%20Conductor%20Limit/set?value=18` (and the others), read with `GET` |
| Restart the ESP32 | publish `PRESS` to `teslawbc/button/restart/command` | `POST /button/Restart/press` |
| Diagnostics: uptime, firmware, IP address | `teslawbc/sensor/uptime/state`, `teslawbc/text_sensor/firmware/state`, `teslawbc/text_sensor/ip_address/state` | `GET /sensor/Uptime`, `/text_sensor/Firmware`, `/text_sensor/IP%20Address` |

**Fallback:** Aperta must set the mode or target, or press Heartbeat, at least once per Fallback Timeout (default 5 min), over either interface. Writing the target every minute is enough. If nothing arrives, Manual and Pause switch to the Fallback Target. This also catches a stalled automation, not only a lost connection.

Mode is restored after a reboot. Target Current is not, to spare the flash: it starts at 6 A until Aperta writes it.

Quick test from a PC:

```sh
mosquitto_sub -h <broker> -t 'teslawbc/#' -v
mosquitto_pub -h <broker> -t teslawbc/number/target_current/command -m 10
curl --digest -u teslawbc:<password> http://<ip>/sensor/Current%20L1
curl --digest -u teslawbc:<password> -X POST 'http://<ip>/select/Mode/set?option=Manual'
```

## Example: charging in the cheap hours

Aperta plans a window, say 01:00 to 05:00. The rest state is **Pause**, so a plugged-in car waits.

| # | When | Aperta does | TeslaWBC and the charger |
|---|---|---|---|
| 1 | Idle | Mode = `Pause` | Reports L + 2 A. The charger does not start a session |
| 2 | Car plugged in | Sees *WC Vehicle Connected* = `ON` and reads the car's state of charge (WiCAN) | Car shows "waiting" |
| 3 | Before the window | Plans current and slots from the price curve and the energy needed. Notes *Energy* as the start value | Waiting |
| 4 | Window opens | Target = `16`, then Mode = `Manual` | The contactor closes within about 25 s, the car ramps to 16 A within 5 to 20 s. State = `Charging` |
| 5 | During the window | Writes the target every minute (also the heartbeat). Lowers it for house load or a pricier slot | Holds each new target within 5 to 20 s |
| 6 | Car full early | Car reports full, or the currents stay below 1 A: Mode = `Pause` | Session ends |
| 7 | Window closes | Mode = `Pause`. Energy charged = *Energy* now minus the start value | The contactor opens within about 10 s. State = `Paused` |
| — | Any time | Alert if *Control Lost* or *Fallback Active* turns `ON` | |

The car's own charging schedule should be off, so the car charges whenever the Wall Connector allows and Aperta alone decides when.

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
4. Leave it a day in **Bypass** mode, then test Manual at 16, 10 and 6 A, then Pause and resume.
5. Optional: the Tesla app allows 5 A, but the Wall Connector's pilot signal (IEC 61851) starts at 6 A. To try 5 A, set Min Current to 5 and Target Current to 5 while charging. If the car settles at 5 A, keep it. If it stays at 6 A and the charger pauses after about 20 s, set Min Current back to 6.
6. Block the Wall Connector's internet access at the router to freeze its firmware (tested on 26.34).

## Credits

The Neurio register map comes from the community reverse engineering in [LucaTNT's gist](https://gist.github.com/LucaTNT/4adf01a7252386559070023612efa117). The charger behaviour notes come from [PVi1/esphome-twc-control](https://github.com/PVi1/esphome-twc-control) and [zany92/tesla-loadpilot](https://github.com/zany92/tesla-loadpilot).

TeslaWBC is not affiliated with Tesla. Changing your wallbox installation is at your own risk, and wiring inside the Wall Connector is electrician work.
