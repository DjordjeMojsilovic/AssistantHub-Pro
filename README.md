# AssistantHub Pro

A desk assistant on an ESP32-S3. It sorts my tasks by priority, tracks habits, runs a Pomodoro timer and answers questions through the Groq API. I control it with a Telegram bot or with a small web UI in the browser. Everything runs on the chip, there is no extra server.

Note: the code comments, the Telegram commands and the UI texts are in German. Change them if you like.

## Features

- **Tasks:** every task gets a score from its type, subject weight and days until the deadline. The list is sorted by this score.
- **AI chat:** Groq API with two modes. Quick uses Llama 3.1 8b, Deep uses Llama 3.3 70b.
- **Telegram bot:** commands, inline keyboards and reminders at fixed times of the day.
- **Web UI:** a single page served by the ESP32. It gets live updates through a WebSocket and can be installed as a PWA.
- **Habits:** 6 habits with streaks. The streaks are saved in SPIFFS, so they stay after a power cut.
- **Pomodoro:** 1 to 4 phases of 25 minutes. Phase changes show on the OLED and come as a Telegram message.
- **OLED:** top 3 tasks, time, WiFi signal, a spinner while the AI works and a screensaver.
- **Dual core:** core 0 does WiFi, HTTP, WebSocket and Telegram. Core 1 does the AI requests, the display and the main loop.
- **Heap handling:** the AI requests use one persistent TLS connection. When the heap gets low or fragmented, the code frees buffers, pauses other services or falls back to the smaller model.

## Hardware

| Part | Note |
|---|---|
| Heltec WiFi Kit 32 V3 (ESP32-S3) | The OLED is on the board |
| USB-C cable | It must be a data cable. A charge-only cable does not work. |
| Breadboard and jumper wires | Only if you want to add LEDs |

The LED code (breathing and blink effects) is in the sketch, but it is commented out.

## Accounts and API keys

### Groq API key (free)

1. Make an account at [groq.com](https://groq.com).
2. Go to `Console > API Keys > Create API Key`.
3. I use two keys, one for the web UI and one for Telegram. Then each one has its own rate limit.

### Telegram bot

1. Open Telegram and write to `@BotFather`.
2. Send `/newbot` and follow the steps.
3. Copy the bot token (format `123456789:AAF...`).
4. Get your chat ID:
   - Send `/start` to your bot once.
   - Open `https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates` in the browser.
   - The `"id"` under `"chat"` is your chat ID.

### WiFi

- SSID and password of your network (2.4 GHz only).
- Optional: SSID and password of a phone hotspot. The ESP32 uses it if the first network does not connect.

## Arduino libraries

Install them with `Sketch > Include Library > Manage Libraries`:

| Library | Version | Name in the Library Manager |
|---|---|---|
| Adafruit SSD1306 | 2.5 or newer | `Adafruit SSD1306` |
| Adafruit GFX | 1.11 or newer | `Adafruit GFX Library` |
| ArduinoJson | 6.21 or newer (v6) | `ArduinoJson` |
| UniversalTelegramBot | 1.3 or newer | `Universal Arduino Telegram Bot` |
| WebSockets | 2.3 or newer | `WebSockets` (Markus Sattler) |

Heltec board package: add this URL under `File > Preferences > Additional Board Manager URLs`:

```
https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/0.0.9/package_heltec_esp32_index.json
```

## Setup

### 1. Credentials

Open `AssistantHubPro.ino` and replace the `YOUR_*` values in the `KONFIGURATION` block (about line 59 to 67):

```cpp
#define BOTtoken          "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID           "YOUR_TELEGRAM_CHAT_ID"
#define GROQ_API_KEY_WEB  "YOUR_GROQ_API_KEY_WEB"
#define GROQ_API_KEY_TG   "YOUR_GROQ_API_KEY_TG"

const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* SSIDhot     = "YOUR_HOTSPOT_SSID";      // optional
const char* Passworthot = "YOUR_HOTSPOT_PASSWORD";
```

Your name is used in three places. Search for `YOUR_NAME`: the welcome screen on the OLED and two AI prompts.

### 2. Arduino IDE

```
Board:            Heltec WiFi Kit 32 V3
Partition Scheme: Default 4MB with spiffs
Upload Speed:     921600
Port:             the COM port of the ESP32
```

### 3. Upload

1. Connect the board with the USB-C data cable.
2. `Sketch > Upload`.
3. Open the Serial Monitor at 115200 baud.
4. After boot the IP address shows in the log and the bot sends it to Telegram.
5. Open `http://<ESP32-IP>/` in the browser.

## Telegram commands

| Command | What it does |
|---|---|
| `/start` | Menu with buttons |
| `/testbot` | Status: RAM, IP, tasks, web UI |
| `/aufgaben` | All tasks with their score |
| `/new` | Add a task in 3 steps |
| `/finish` | Mark a task as done |
| `/ask <question>` | Short answer, Llama 3.1 8b |
| `/deep <topic>` | Longer answer with headings, Llama 3.3 70b |
| `/checkout` | Lock the web UI, use only Telegram |
| `/heap` | RAM details for debugging |
| `/uptime` | Time since the last boot |
| `/restart` | Restart the ESP32 after 2 seconds |

The Pomodoro timer starts from the web UI.

## How it works

```
Core 0 (networkTaskLogic)        Core 1 (aiTask and loop)
  WiFi reconnect                   AI requests to Groq (TLS)
  HTTP server, port 80             OLED and screensaver
  WebSocket, port 81               Pomodoro timer
  Telegram polling, 400 ms         Heap log every 30 s
  Delivers the AI answers

SPIFFS
  /habits.json   habit streaks
```

AI requests go through one job slot (`aiJob`). The web UI or Telegram puts a job in, `aiTask` sends it to Groq, and the network task delivers the answer.

The heap is the weak point, so the code checks it before each request:

1. When a job comes in with a fragmented heap, `requestAI()` uses the 8b model or fewer tokens.
2. If the heap is low, free the cached last answer and wait so the allocator can merge free blocks.
3. If the largest free block is below 38 KB, pause Telegram and the WebSocket until the request is done.
4. If it is still below 38 KB, log the details and wait up to 10 seconds before the TLS request.

## Troubleshooting

**The board is not found**
- Use a USB-C data cable.
- Install the [CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).
- On Windows, look for the COM port in the Device Manager.

**AI requests fail (HTTP -1)**
- Check the Groq keys in the `#define` lines.
- Look at the `[AI]` and `[AI-POST]` lines in the Serial Monitor.
- Send `/restart` in Telegram. If that does not help, unplug the board and plug it in again.

**WiFi does not connect**
- Check SSID and password, including upper and lower case.
- The ESP32 only supports 2.4 GHz, not 5 GHz.
- Add the hotspot as a fallback.

**SPIFFS errors**
- Set `Tools > Partition Scheme > Default 4MB with spiffs`.
- Erase the flash with `Tools > Erase Flash > All Flash Contents` and upload again.

## License

MIT, see [LICENSE](LICENSE).

Made by [Djordje Mojsilovic](https://github.com/DjordjeMojsilovic), 2026.
