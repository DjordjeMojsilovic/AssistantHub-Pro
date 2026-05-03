<div align="center">

# 🤖 AssistantHub Pro

**An ESP32-S3 desk assistant with AI, Telegram bot, Web-UI, habit tracking and more.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-orange?style=for-the-badge&logo=espressif)](https://www.espressif.com/)
[![Language](https://img.shields.io/badge/Language-Arduino%20C%2B%2B-teal?style=for-the-badge&logo=arduino)](https://www.arduino.cc/)
[![AI](https://img.shields.io/badge/AI-Groq%20%7C%20Llama%203-purple?style=for-the-badge)](https://groq.com)
[![Uptime](https://img.shields.io/badge/Software%20Uptime-96.4%25-brightgreen?style=for-the-badge)]()
[![Lines](https://img.shields.io/badge/Lines%20of%20Code-3368-lightgrey?style=for-the-badge)]()

[![FreeRTOS](https://img.shields.io/badge/FreeRTOS-Dual--Core-blue?style=flat-square)](https://freertos.org)
[![Telegram](https://img.shields.io/badge/Telegram-Bot-2CA5E0?style=flat-square&logo=telegram)](https://core.telegram.org/bots)
[![WebSocket](https://img.shields.io/badge/WebSocket-Real--Time-green?style=flat-square)]()
[![SPIFFS](https://img.shields.io/badge/Storage-SPIFFS-yellow?style=flat-square)]()

*Built with passion, too much coffee and Claude Pro.*

</div>

---

## What is AssistantHub Pro?

AssistantHub Pro turns an ESP32-S3 microcontroller into a smart desk assistant. It runs 24/7, automatically prioritizes your tasks, answers questions via AI and can be controlled via Telegram as well as a browser-based Web-UI — all running locally on the chip, no server required.

```
┌─────────────────────────────────────────────────────────────┐
│                    AssistantHub Pro                         │
│                                                             │
│   📱 Telegram          🌐 Web-UI (Browser)                  │
│        ↓                      ↓                            │
│   ┌────────────────────────────────┐                        │
│   │        ESP32-S3 Chip           │                        │
│   │  Core 0: Network & Telegram    │                        │
│   │  Core 1: AI & Display & Logic  │                        │
│   └────────────────────────────────┘                        │
│        ↓              ↓           ↓                         │
│   📺 OLED         💾 SPIFFS    🤖 Groq API                  │
└─────────────────────────────────────────────────────────────┘
```

---

## ✨ Features

| | Feature | Description |
|---|---|---|
| 📋 | **Task Manager** | Automatic prioritization via a custom scoring algorithm |
| 🤖 | **AI Chat** | Groq API – Quick Mode (Llama 3.1 8b) & Deep Mode (Llama 3.3 70b) |
| 📱 | **Telegram Bot** | Full control via Telegram – commands, inline keyboards, daily reminders |
| 🌐 | **Web-UI** | Responsive PWA via WebSocket – real-time updates without page reload |
| 🏆 | **Habit Tracking** | 6 habits with streak counter, persisted via SPIFFS flash |
| ⏱️ | **Pomodoro Timer** | 1–4 phases, OLED display + Telegram notification on phase change |
| 📺 | **OLED Display** | Top-3 tasks, time, WiFi RSSI bar, AI spinner, screensaver |
| 💾 | **Persistence** | Habits survive power cuts thanks to SPIFFS flash storage |
| 🔄 | **Dual-Core** | FreeRTOS – Core 0: networking, Core 1: UI and AI logic |
| 🛡️ | **Heap Management** | 3-tier system + persistent TLS connection (saves ~40KB RAM per request) |

---

## 🔧 Required Hardware

| Component | Notes |
|---|---|
| **Heltec ESP32-S3 WiFi Kit V3** | OLED already built in ✅ |
| Dupont Jumper Wires | Male-to-Male |
| Breadboard | Standard 830-point |
| **USB-C Data Cable** | ⚠️ Must be a data cable – charging-only cables won't work! |

> 💡 **Lite Edition:** This version does not include LED control.
> For the full version with RGB LEDs (breathing/blink) → branch `full-version`

---

## 🔑 Required APIs & Accounts

<details>
<summary><b>1. Groq API Key (free)</b></summary>

1. Create an account at [groq.com](https://groq.com)
2. Generate an API key: `Console → API Keys → Create API Key`
3. **Two keys recommended** – one for Web-UI, one for Telegram (separate rate-limit tracking)
4. Free daily limit: ~14,400 requests (8b model)

</details>

<details>
<summary><b>2. Telegram Bot</b></summary>

1. Open Telegram → search for `@BotFather`
2. Send `/newbot` and follow the instructions
3. Note your **Bot Token** (format: `123456789:AAF...`)
4. Find your **Chat ID**:
   - Start your bot once with `/start`
   - Open in browser: `https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates`
   - The `"id"` value under `"chat"` is your Chat ID

</details>

<details>
<summary><b>3. WiFi Credentials</b></summary>

- SSID + password of your home network
- Optional: SSID + password of a mobile hotspot as automatic fallback

</details>

---

## 📦 Required Arduino Libraries

Install via `Sketch → Include Library → Manage Libraries`:

| Library | Min. Version | Library Manager Name |
|---|---|---|
| Adafruit SSD1306 | ≥ 2.5 | `Adafruit SSD1306` |
| Adafruit GFX | ≥ 1.11 | `Adafruit GFX Library` |
| ArduinoJson | ≥ 6.21 | `ArduinoJson` |
| UniversalTelegramBot | ≥ 1.3 | `Universal Arduino Telegram Bot` |
| WebSockets | ≥ 2.3 | `WebSockets` (by Markus Sattler) |

**Heltec Board Manager URL** – paste under `File → Preferences → Additional Board Manager URLs`:
```
https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/0.0.9/package_heltec_esp32_index.json
```

---

## 🚀 Setup & Installation

### Step 1 – Fill in your credentials

Open `AssistantHubPro.ino` and replace all `YOUR_*` placeholders (around line 73–81):

```cpp
// ─── REQUIRED: Fill in before uploading ─────────────────────
#define BOTtoken          "YOUR_TELEGRAM_BOT_TOKEN"   // From @BotFather
#define CHAT_ID           "YOUR_TELEGRAM_CHAT_ID"     // Your Telegram Chat ID
#define GROQ_API_KEY_WEB  "YOUR_GROQ_API_KEY_WEB"     // Groq key for Web-UI
#define GROQ_API_KEY_TG   "YOUR_GROQ_API_KEY_TG"      // Groq key for Telegram

const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* SSIDhot     = "YOUR_HOTSPOT_SSID";        // Optional fallback
const char* Passworthot = "YOUR_HOTSPOT_PASSWORD";
```

Also enter your name (around line ~2529 and ~2676):
```cpp
display.println("YOUR_NAME");           // Welcome text on OLED
"My name is YOUR_NAME. ..."             // AI prompt personalization
```

### Step 2 – Configure Arduino IDE

```
Board:            Heltec WiFi Kit 32 V3
Partition Scheme: Default 4MB with spiffs
Upload Speed:     921600
Port:             COM port of your ESP32
```

### Step 3 – Upload & start

```
1. Connect ESP32 via USB-C data cable
2. Sketch → Upload
3. Tools → Serial Monitor (Baud: 115200)
4. After boot: find the IP address in the log
5. Open in browser: http://[ESP32-IP]/
```

---

## 📱 Telegram Commands

| Command | Description |
|---|---|
| `/start` | Welcome menu with quick-access buttons |
| `/testbot` | System status: RAM, IP, tasks, WiFi |
| `/aufgaben` | All tasks with priority score |
| `/new` | Create new task – guided 3-step flow |
| `/finish` | Mark task as done (inline keyboard) |
| `/ask <question>` | Quick AI question – Llama 3.1 8b |
| `/deep <topic>` | In-depth research – Llama 3.3 70b |
| `/pomodoro` | Start a Pomodoro session |
| `/checkout` | Lock Web-UI (Telegram-only mode) |
| `/heap` | RAM diagnostics for debugging |
| `/uptime` | Show system uptime |
| `/restart` | Safe restart (RTOS-safe, flag-based) |

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Core 0 – Network Task          Core 1 – AI & Main Loop     │
│  ├── WiFi Auto-Reconnect        ├── OLED Display Rendering  │
│  ├── Telegram Polling (400ms)   ├── Pomodoro Tick           │
│  ├── HTTP Server (Port 80)      ├── Screensaver (10s)       │
│  ├── WebSocket Loop (Port 81)   ├── AI Execution (Groq TLS) │
│  └── AI Response Delivery       └── Heap Management         │
├─────────────────────────────────────────────────────────────┤
│  SPIFFS Flash                                               │
│  └── /habits.json  →  Habit streaks (persistent)           │
├─────────────────────────────────────────────────────────────┤
│  Heap Management – 3-Tier System                            │
│  Tier 1: Free AI buffer + yield loops                       │
│  Tier 2: Pause services (Critical TLS Mode)                 │
│  Tier 3: Diagnostics + Degraded Mode (8b instead of 70b)   │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Troubleshooting

<details>
<summary><b>Board not recognized</b></summary>

- Use a USB-C **data cable** – charging-only cables have no data lines
- Install CP210x VCP driver: [Silicon Labs](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- Windows: check Device Manager to see if ESP32 appears as a COM port

</details>

<details>
<summary><b>AI requests failing (HTTP -1)</b></summary>

- Double-check your Groq API keys in the `#define` lines
- Open Serial Monitor → watch for `[AI]` log lines
- Restart the ESP32 via `/restart` in Telegram
- If the issue persists: unplug and replug the chip

</details>

<details>
<summary><b>WiFi not connecting</b></summary>

- Check SSID and password for typos and capitalization
- Configure the hotspot fallback as a backup network
- ESP32 only supports 2.4 GHz networks, not 5 GHz

</details>

<details>
<summary><b>SPIFFS errors</b></summary>

- Set `Tools → Partition Scheme → Default 4MB with spiffs`
- Erase flash: `Tools → Erase Flash → All Flash Contents`
- Then re-upload the sketch

</details>

---

## 📄 License

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

MIT License – free to use, modify and redistribute with attribution to the original author.

---

<div align="center">

Built by **[Djordje Mojsilovic](https://github.com/DjordjeMojsilovic)** | 2026

[![GitHub](https://img.shields.io/badge/GitHub-DjordjeMojsilovic-181717?style=flat-square&logo=github)](https://github.com/DjordjeMojsilovic)

*If you find this project useful – a ⭐ means a lot!*

</div>
