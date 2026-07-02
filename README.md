# FridgeGuard IoT

**Smart refrigerator monitoring system — ESP32-C3 SuperMini**
ESIB Engineering Internship · Summer 2026

FridgeGuard turns an ordinary refrigerator into a connected device: door monitoring, temperature tracking, usage statistics, a local OLED dashboard, remote Telegram alerts, and live cloud sync — built on inexpensive hardware for under $15 in components.

---

## Table of Contents

1. [Features](#features)
2. [Hardware](#hardware)
3. [Power Options](#power-options)
4. [Architecture](#architecture)
5. [Repository Structure](#repository-structure)
6. [Setup — Your Own Credentials](#setup--your-own-credentials)
7. [Flashing the Firmware](#flashing-the-firmware)
8. [Multi-Device Support](#multi-device-support)
9. [Firmware Variants Explained](#firmware-variants-explained)
10. [Test Sketches](#test-sketches)
11. [Known Limitations](#known-limitations)
12. [Credits](#credits)

---

## Features

| # | Feature | Description |
|---|---|---|
| F1 | Door Monitoring | Reed switch detects every open/close event with exact timing |
| F2 | Long-Open Alert | Alert fires if the door stays open past 60 seconds |
| F3 | Temperature Monitoring | DS18B20 probe reads continuously; alerts above threshold |
| F4 | Repeating Alerts | Door and temperature alerts repeat on a timer while the condition persists, with hysteresis |
| F5 | Telegram Alerts | Instant phone notifications for door, temperature, and system events |
| F6 | OLED Dashboard | 6 screens cycling via button: Door/Clock, Live Status, Stats, Events, Alerts, System Info |
| F7 | Daily Statistics | Openings, total/longest open time, temperature min/max, thermal impact |
| F8 | Weekly Rolling Summary | 7-day rolling averages, uploaded to Firestore for the companion app |
| F9 | Flash Persistence | Daily and weekly stats survive power loss via ESP32 NVS (Preferences) |
| F10 | Maintenance Mode | Pause all alerts during cleaning/repair — physical hold, Telegram command, or app control |
| F11 | Cloud Sync | Firebase Firestore upload every 12 seconds + instantly on door close |
| F12 | Multi-Device Support | Each board gets its own Firestore document automatically via its factory MAC address |
| F13 | Auto-Reconnect | WiFi drop recovery with no manual reset needed |

---

## Hardware

| Component | Spec | Approx. Cost |
|---|---|---|
| ESP32-C3 SuperMini | Built-in 0.42" 72×40 OLED | $4–6 |
| DS18B20 | Waterproof probe | $2–3 |
| Magnetic Reed Switch | Normally Open (NO) | $0.50–1 |
| 4.7kΩ Resistor | Pull-up for DS18B20 | $0.05 |

**Pin Reference**

| GPIO | Connected To | Mode |
|---|---|---|
| 3 | Reed switch signal | `INPUT_PULLUP` + interrupt |
| 4 | DS18B20 data | OneWire |
| 5 | OLED SDA (internal) | I2C |
| 6 | OLED SCL (internal) | I2C |
| 8 | Onboard LED | Output (heartbeat) |
| 9 | BOOT button | `INPUT_PULLUP` + interrupt |

Full wiring diagrams and pin-level detail are covered inline in each sketch's comments.

---

## Power Options

FridgeGuard can run two ways:

| Mode | How | Use Case |
|---|---|---|
| **USB-C direct** | 5V straight from a USB cable or wall adapter | Development, testing, permanent wall power (current default) |
| **Battery-backed** | LiPo → TP4056 charger/protection → MT3608 boost converter → 5V pin | Slim, permanent mount with automatic power-loss backup |

The battery-backed design (bill of materials, wiring steps, MT3608 voltage calibration procedure, and a battery-percentage display via a GPIO 0 voltage divider) has been fully specified and validated on hardware.

**One safety rule that applies regardless of mode:** never connect both the USB-C port and the 5V pin at once — the board has no circuit to safely arbitrate two simultaneous power sources, and doing so risks permanent damage.

---

## Architecture

```
Physical Event  →  Sensor Layer  →  ESP32-C3  →  OLED / Telegram / Firestore
(door, temp)       (reed switch,     (logic,       (local display, phone alert,
                    DS18B20)          state,        cloud sync)
                                      stats)
```

Each board computes a unique `deviceId` from its factory-burned MAC address (`ESP.getEfuseMac()`) at boot. This means the **same firmware file** can be flashed onto multiple boards, and each one writes to its own Firestore document automatically — no manual configuration, no collisions. See [Multi-Device Support](#multi-device-support).

---

## Repository Structure

```
FridgeGuard-IoT/
├── firmware/
│   ├── FridgeGuard_Final/                    ← Full-featured, production sketch
│   ├── FridgeGuard_NoFirebase_Maintenance/    ← Telegram + Maintenance Mode, no cloud sync
│   ├── FridgeGuard_CoreOnly/                  ← Sensors + OLED only, no networking
│   └── archive/
│       └── FridgeGuard_v1/                    ← Earlier milestone, kept for history
├── tests/
│   ├── 01_wifi_connection/
│   ├── 02_ntp_clock/
│   ├── 03_reed_switch/
│   ├── 04_temperature_ds18b20/
│   ├── 05_oled_display/
│   └── 06_telegram_bot/
├── .gitignore
└── README.md
```

`tests/` is numbered to match the actual build order (Week 1 → Week 3): each folder isolates and validates a single subsystem before it gets integrated into the full firmware. `firmware/` holds complete, deployable builds.

---

## Setup — Your Own Credentials

This project keeps real WiFi, Telegram, and Firebase credentials **out of the repository** for security. Every sketch that needs them ships with a `secrets_template.h` instead of live values.

Before uploading **any** sketch:

1. Open the sketch folder you're using (e.g. `firmware/FridgeGuard_Final/`).
2. Copy `secrets_template.h` and rename the copy to `secrets.h`, in the same folder.
3. Open `secrets.h` and fill in your own values:

   | Macro | Where to get it |
   |---|---|
   | `WIFI_SSID` / `WIFI_PASSWORD` | Your 2.4GHz WiFi network name and password |
   | `BOT_TOKEN` | From [@BotFather](https://t.me/BotFather) on Telegram — `/newbot` |
   | `CHAT_ID` | Message your bot, then visit `https://api.telegram.org/bot<TOKEN>/getUpdates` and read the `chat.id` field |
   | `FIREBASE_PROJECT_ID` / `FIREBASE_API_KEY` | Firebase Console → Project Settings (Final sketch only) |

4. `secrets.h` is listed in `.gitignore` — it will never be committed or pushed. It's safe to put real credentials there.
5. Compile and upload as normal.

**Not every sketch needs every credential** — check that folder's `secrets_template.h` for the exact set it expects. `03_reed_switch`, `04_temperature_ds18b20`, and `05_oled_display` need no credentials at all.

---

## Flashing the Firmware

1. Install **Arduino IDE 2.x** and the ESP32 board package (Boards Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
2. Select **Tools → Board → ESP32 Arduino → ESP32C3 Dev Module**
3. Set **Tools → USB CDC On Boot → Enabled** (required for Serial Monitor output)
4. Complete the [credentials setup](#setup--your-own-credentials) for the sketch you're flashing
5. Connect via a USB-C **data** cable (not charge-only)
6. Upload, then open **Serial Monitor** at **115200 baud** to watch boot output

---

## Multi-Device Support

Every board that runs `FridgeGuard_Final` prints its own unique device ID to Serial Monitor on boot:

```
This device's unique ID: A1B2C3D4E5F6
```

This ID is generated from the chip's factory MAC address — it's different on every physical board, requires no configuration, and determines which Firestore document that board's data is written to (`fridge_data/{deviceId}`, `daily_stats/{deviceId}/...`, `weekly_summary/{deviceId}`).

To run FridgeGuard on multiple fridges: flash the identical `FridgeGuard_Final` sketch (with each builder's own `secrets.h`) onto each board. Each board's data stays completely separate in Firestore automatically. Note this device ID down after first boot — it's what identifies that specific fridge in the companion app.

---

## Firmware Variants Explained

| Sketch | WiFi | Telegram | Firebase | Maintenance Mode | Notes |
|---|:---:|:---:|:---:|:---:|---|
| `FridgeGuard_Final` | ✅ | ✅ | ✅ | ✅ (app + Telegram + button) | Complete, production build |
| `FridgeGuard_NoFirebase_Maintenance` | ✅ | ✅ | ❌ | ✅ (button only) | Full alerting, no cloud sync |
| `FridgeGuard_CoreOnly` | ✅ | ❌ | ❌ | ❌ | Sensors + OLED only, for isolated testing |
| `archive/FridgeGuard_v1` | ✅ | ✅ | ✅ | ❌ | Earlier milestone, superseded by Final |

---

## Test Sketches

Built in order, each validating one subsystem before integration — the methodology this project follows throughout:

| Folder | Validates |
|---|---|
| `01_wifi_connection` | WiFi join + IP address |
| `02_ntp_clock` | Internet time sync |
| `03_reed_switch` | Door open/close interrupt logic |
| `04_temperature_ds18b20` | Temperature sensor readings |
| `05_oled_display` | OLED init and drawing |
| `06_telegram_bot` | Sending a Telegram message from the board |

---

## Known Limitations

- Firestore free (Spark) tier write limits require the 12-second upload interval to stay within daily quota across multiple boards.
- ESP32 chips support 2.4GHz WiFi only — no 5GHz networks.

---

## Credits

Built by Dima Jomaa, Fatima Hamze, and Sophia Antoun as part of the ESIB (École Supérieure d'Ingénieurs de Beyrouth) Summer 2026 Engineering Internship.

*For educational use — ESIB Engineering Internship, Summer 2026.*
