<div align="center">

# 🤖 AssistantHub Pro

**Ein ESP32-S3 Schreibtisch-Assistent mit KI, Telegram-Bot, Web-UI und mehr.**

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

*Gebaut mit Herzblut, zu viel Kaffee und Claude Pro.*

</div>

---

## Was ist AssistantHub Pro?

AssistantHub Pro verwandelt einen ESP32-S3 Mikrocontroller in einen smarten Schreibtisch-Assistenten. Er läuft 24/7, priorisiert Aufgaben automatisch, beantwortet Fragen via KI und ist sowohl über Telegram als auch über eine Web-Oberfläche im Browser steuerbar – alles lokal auf dem Chip, ohne eigenen Server.

```
┌─────────────────────────────────────────────────────────────┐
│                    AssistantHub Pro                         │
│                                                             │
│   📱 Telegram          🌐 Web-UI (Browser)                  │
│        ↓                      ↓                            │
│   ┌────────────────────────────────┐                        │
│   │        ESP32-S3 Chip           │                        │
│   │  Core 0: Netzwerk & Telegram   │                        │
│   │  Core 1: KI & Display & Logic  │                        │
│   └────────────────────────────────┘                        │
│        ↓              ↓           ↓                         │
│   📺 OLED         💾 SPIFFS    🤖 Groq API                  │
└─────────────────────────────────────────────────────────────┘
```

---

## ✨ Features

| | Feature | Beschreibung |
|---|---|---|
| 📋 | **Task-Manager** | Automatische Priorisierung via eigenem Scoring-Algorithmus |
| 🤖 | **KI-Chat** | Groq API – Quick Modus (Llama 3.1 8b) & Deep Modus (Llama 3.3 70b) |
| 📱 | **Telegram Bot** | Vollständige Steuerung – Commands, InlineKeyboards, tägliche Reminder |
| 🌐 | **Web-UI** | Responsive PWA via WebSocket – Echtzeit-Updates ohne Reload |
| 🏆 | **Habit Tracking** | 6 Habits mit Streak-Zähler, persistiert via SPIFFS Flash |
| ⏱️ | **Pomodoro Timer** | 1–4 Phasen, OLED-Anzeige + Telegram-Notification |
| 📺 | **OLED Display** | Top-3-Tasks, Uhrzeit, WiFi-RSSI-Bar, KI-Spinner, Screensaver |
| 💾 | **Persistenz** | Habits überleben Stromausfälle dank SPIFFS Flash-Speicher |
| 🔄 | **Dual-Core** | FreeRTOS – Core 0: Netzwerk, Core 1: UI und KI-Logik |
| 🛡️ | **Heap-Management** | 3-Tier System + persistente TLS-Verbindung (spart 40KB RAM pro Anfrage) |

---

## 🔧 Benötigte Hardware

| Komponente | Hinweis |
|---|---|
| **Heltec ESP32-S3 WiFi Kit V3** | OLED bereits eingebaut ✅ |
| Jumperkabel (Dupont) | Male-to-Male |
| Breadboard | Standard 830-Punkt |
| **USB-C Datenkabel** | ⚠️ Muss Datenkabel sein – Ladekabel funktionieren nicht! |

> 💡 **Lite Edition:** Diese Version enthält keine LED-Steuerung.
> Für die Vollversion mit RGB-LEDs → Branch `full-version`

---

## 🔑 Benötigte APIs & Accounts

<details>
<summary><b>1. Groq API Key (kostenlos)</b></summary>

1. Account erstellen auf [groq.com](https://groq.com)
2. API Key generieren: `Console → API Keys → Create API Key`
3. **Zwei Keys empfohlen** – einen für Web-UI, einen für Telegram
4. Kostenloses Tageslimit: ~14'400 Anfragen (8b Modell)

</details>

<details>
<summary><b>2. Telegram Bot</b></summary>

1. Öffne Telegram → suche `@BotFather`
2. Sende `/newbot` und folge den Anweisungen
3. **Bot Token** notieren (Format: `123456789:AAF...`)
4. **Chat ID** herausfinden:
   - Starte deinen Bot einmal mit `/start`
   - Öffne: `https://api.telegram.org/bot<DEIN_TOKEN>/getUpdates`
   - Die `"id"` unter `"chat"` ist deine Chat ID

</details>

<details>
<summary><b>3. WiFi Credentials</b></summary>

- SSID + Passwort deines Heimnetzwerks
- Optional: SSID + Passwort eines Hotspots als automatischer Fallback

</details>

---

## 📦 Benötigte Arduino Libraries

Installiere über `Sketch → Include Library → Manage Libraries`:

| Library | Min. Version | Library Manager Name |
|---|---|---|
| Adafruit SSD1306 | ≥ 2.5 | `Adafruit SSD1306` |
| Adafruit GFX | ≥ 1.11 | `Adafruit GFX Library` |
| ArduinoJson | ≥ 6.21 | `ArduinoJson` |
| UniversalTelegramBot | ≥ 1.3 | `Universal Arduino Telegram Bot` |
| WebSockets | ≥ 2.3 | `WebSockets` (by Markus Sattler) |

**Heltec Board Manager URL** – einfügen unter `File → Preferences → Additional Board Manager URLs`:
```
https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/0.0.9/package_heltec_esp32_index.json
```

---

## 🚀 Setup & Installation

### Schritt 1 – Platzhalter ausfüllen

Öffne `AssistantHubPro.ino` und ersetze alle `YOUR_*` Werte (Zeile ~73–81):

```cpp
// ─── PFLICHT: Diese Werte vor dem Upload ausfüllen ───────────
#define BOTtoken          "YOUR_TELEGRAM_BOT_TOKEN"   // Von @BotFather
#define CHAT_ID           "YOUR_TELEGRAM_CHAT_ID"     // Deine Telegram Chat ID
#define GROQ_API_KEY_WEB  "YOUR_GROQ_API_KEY_WEB"     // Groq Key für Web-UI
#define GROQ_API_KEY_TG   "YOUR_GROQ_API_KEY_TG"      // Groq Key für Telegram

const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* SSIDhot     = "YOUR_HOTSPOT_SSID";        // Fallback optional
const char* Passworthot = "YOUR_HOTSPOT_PASSWORD";
```

Deinen Namen eintragen (Zeile ~2529 und ~2676):
```cpp
display.println("YOUR_NAME");           // Willkommenstext auf OLED
"Mein Name ist YOUR_NAME. ..."          // KI-Prompt Personalisierung
```

### Schritt 2 – Arduino IDE konfigurieren

```
Board:            Heltec WiFi Kit 32 V3
Partition Scheme: Default 4MB with spiffs
Upload Speed:     921600
Port:             COM-Port des ESP32
```

### Schritt 3 – Hochladen & starten

```
1. ESP32 via USB-C Datenkabel anschliessen
2. Sketch → Upload
3. Tools → Serial Monitor (Baud: 115200)
4. Nach Boot: IP-Adresse aus Log entnehmen
5. Browser öffnen: http://[ESP32-IP]/
```

---

## 📱 Telegram Commands

| Command | Beschreibung |
|---|---|
| `/start` | Willkommensmenü mit Quick-Access Buttons |
| `/testbot` | System-Status: RAM, IP, Tasks, WiFi |
| `/aufgaben` | Alle Tasks mit Prioritäts-Score |
| `/new` | Neue Aufgabe – geführter 3-Schritt Flow |
| `/finish` | Task als erledigt markieren (InlineKeyboard) |
| `/ask <frage>` | Schnelle KI-Frage – Llama 3.1 8b |
| `/deep <thema>` | Tiefenrecherche – Llama 3.3 70b |
| `/pomodoro` | Pomodoro-Session starten |
| `/checkout` | Web-UI sperren (Telegram-Only Modus) |
| `/heap` | RAM-Diagnostik für Debugging |
| `/uptime` | System-Laufzeit anzeigen |
| `/restart` | Sicherer Neustart (RTOS-safe) |

---

## 🏗️ Architektur

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
│  └── /habits.json  →  Habit Streaks (persistent)           │
├─────────────────────────────────────────────────────────────┤
│  Heap Management – 3-Tier System                            │
│  Tier 1: AI-Buffer freigeben + yield                        │
│  Tier 2: Services pausieren (Critical TLS Mode)             │
│  Tier 3: Diagnostik + Degraded Mode (8b statt 70b)         │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Troubleshooting

<details>
<summary><b>Board wird nicht erkannt</b></summary>

- USB-C **Datenkabel** verwenden – Ladekabel haben keine Datenleitungen
- CP210x VCP Treiber installieren: [Silicon Labs](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- Windows: Geräte-Manager prüfen ob ESP32 als COM-Port erscheint

</details>

<details>
<summary><b>KI-Anfragen schlagen fehl (HTTP -1)</b></summary>

- Groq API Key in den `#define` Zeilen prüfen
- Serial Monitor öffnen → `[AI]` Logs beobachten
- ESP32 neu starten: `/restart` via Telegram
- Bei dauerhaften Fehlern: Chip kurz vom Strom trennen

</details>

<details>
<summary><b>WiFi verbindet nicht</b></summary>

- SSID und Passwort auf Gross/Kleinschreibung prüfen
- Hotspot-Fallback konfigurieren
- Nur 2.4 GHz Netzwerke werden unterstützt

</details>

<details>
<summary><b>SPIFFS Fehler</b></summary>

- `Tools → Partition Scheme → Default 4MB with spiffs`
- Flash löschen: `Tools → Erase Flash → All Flash Contents`
- Danach neu hochladen

</details>

---

## 📄 Lizenz

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

MIT License – frei verwendbar, veränderbar und weiterverteilbar mit Nennung des Autors.

---

<div align="center">

Erstellt von **[Djordje Mojsilovic](https://github.com/DjordjeMojsilovic)** | 2026

[![GitHub](https://img.shields.io/badge/GitHub-DjordjeMojsilovic-181717?style=flat-square&logo=github)](https://github.com/DjordjeMojsilovic)

*Falls dir dieses Projekt gefällt – ein ⭐ freut mich sehr!*

</div>
