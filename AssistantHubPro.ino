// ============================================================
//  ASSISTANT HUB PRO - Public Release (Lite Edition)
//  GitHub: https://github.com/YOUR_GITHUB_USERNAME/AssistantHubPro
//
//  Beschreibung:
//  Ein ESP32-S3 basierter Produktivitäts-Assistent mit:
//  - KI-Chat via Groq API (Llama 3.1 8b / 3.3 70b)
//  - Telegram Bot Steuerung
//  - Web-UI via WebSocket (Port 81) + HTTP (Port 80)
//  - Task-Priorisierungs-Algorithmus
//  - Habit-Tracking mit Streak-Persistierung (SPIFFS)
//  - Pomodoro-Timer
//  - OLED Display (128x64, SSD1306)
//  - FreeRTOS Dual-Core Architektur
//
//  Hardware: Heltec ESP32-S3 WiFi Kit V3 (mit eingebautem OLED)
//
//  SETUP:
//  1. Alle YOUR_* Platzhalter unten ausfüllen (Zeilen ~52-60)
//  2. Benötigte Libraries installieren (siehe README.md)
//  3. Board: "Heltec WiFi Kit 32 V3" in Arduino IDE auswählen
//  4. Flash Size: 4MB, Partition Scheme: "Default 4MB with spiffs"
//
//  HINWEIS - Lite Edition:
//  Diese Version enthält KEINE LED-Steuerung.
//  Für die vollständige Version mit RGB-LEDs (Breathing/Blink)
//  siehe Branch "full-version" im Repository.
//
//  Lizenz: MIT License
//  Autor: YOUR_NAME | Jahr: 2026
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
// [BLE-REMOVED] BLE complete removal - frees ~30-40 KB heap for AI/TLS
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include "time.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ───────────────────────────────────────────────────────────
// ROOT CERTIFICATE (Cloudflare R3 for api.groq.com)
// ───────────────────────────────────────────────────────────

// ───────────────────────────────────────────────────────────
// FORWARD DECLARATIONS (KORRIGIERT)
// ───────────────────────────────────────────────────────────
void wakeUpDisplay();
void updateDisplay();
void processTask(const String& cat, float weight, int days, const String& fachName);
bool requestAI(int type, const String& prompt, const String& model, int telMsgId = -1);
void saveHabits();  // [DEMO-FEATURE] Forward declaration
void loadHabits();  // [DEMO-FEATURE] Forward declaration
void triggerEventBlink();

// Behebt den Fehler mit 'lastUpdate'
unsigned long lastUpdate = 0; 
// ───────────────────────────────────────────────────────────

// ─── KONFIGURATION ───────────────────────────────────────────
#define BOTtoken          "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID           "YOUR_TELEGRAM_CHAT_ID"
#define GROQ_API_KEY_WEB  "YOUR_GROQ_API_KEY_WEB"
#define GROQ_API_KEY_TG   "YOUR_GROQ_API_KEY_TG"

const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* SSIDhot     = "YOUR_HOTSPOT_SSID";
const char* Passworthot = "YOUR_HOTSPOT_PASSWORD";

// ─── HARDWARE PINS ───────────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      21
#define SCREEN_ADDRESS  0x3C
#define SDA_PIN         17
#define SCL_PIN         18
// ─── LED PINS (Lite Edition: LEDs deaktiviert) ──────────────
// Um LEDs zu aktivieren: Pins einkommentieren und LedController
// Aufrufe in setup() und loop() einkommentieren.
// #define LED_GREEN  2    // Grüne LED – Idle Breathing
// #define LED_RED    19   // Rote LED  – Pomodoro Breathing
// #define LED_BLUE   5    // Blaue LED – Event Blink (neuer Task etc.)
// const int ledPin = 4;   // Status-LED
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── NETZWERK ────────────────────────────────────────────────
WiFiClientSecure     client;
UniversalTelegramBot bot(BOTtoken, client);
WebServer            server(80);
WebSocketsServer     webSocket(81);

// [SOCKET-RESET] Persistent TLS for AI requests with periodic reset
WiFiClientSecure     persistentAiClient;
volatile bool        tlsHealthy = false;
volatile int         requestCountSinceReset = 0;
const int            RESET_SOCKET_AFTER = 15; 

const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 3600;
const int   daylightOffset_sec = 3600;

// ─── SEMAPHORE / MUTEX ───────────────────────────────────────
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t aiJobMutex;

// ─── AI JOB ──────────────────────────────────────────────────
#define AI_PROMPT_MAX  800
#define AI_RESULT_MAX  5000
#define AI_MODEL_MAX    64
#define AI_JOB_NONE     0
#define AI_JOB_WEB      1
#define AI_JOB_TELEGRAM 2

struct AiJobState {
  char prompt[AI_PROMPT_MAX];
  char model [AI_MODEL_MAX];
  int  type;
  int  telMsgId;
  char result[AI_RESULT_MAX];  // [FIX2] Static array (was char* malloc) – prevents heap fragmentation
  bool pending;
  bool done;
  bool heapWasLow;         // [LOW-HEAP-UX] true → aiTask runs aggressiveHeapCleanup()
  bool degradedMode;       // [OPTION3] true → running with smaller model/reduced tokens
  bool tokenReductionApplied;  // [OPTION3] true → use 300 tokens instead of 600
} aiJob;

TaskHandle_t NetworkTask = NULL;
TaskHandle_t AITask      = NULL;

// [BULLETPROOF-TIER2] Global control variables for critical TLS mode
volatile bool tlsCriticalMode = false;          // true when MaxAllocHeap < 38KB – pause services
volatile bool telegramTaskEnabled = true;       // false during critical TLS mode
volatile bool webSocketTaskEnabled = true;      // false during critical TLS mode
volatile bool networkTaskEnabled = true;        // false during critical TLS mode

// [FIX-WEBUI] Boot-Benachrichtigung wird an networkTask delegiert,
// damit setup() nicht durch TLS-Handshakes blockiert wird bevor server.handleClient() läuft.
volatile bool bootNotifyPending = false;
// [ENGINEER-FIX1] Mutex for atomic bootNotifyPending access on dual-core ESP32-S3
portMUX_TYPE bootNotifyMutex = portMUX_INITIALIZER_UNLOCKED;

// ─── TIMING & POMODORO ───────────────────────────────────────
volatile unsigned long lastTelegramCheck   = 0;
volatile unsigned long lastInteractionTime = 0;
unsigned long          lastBotClientReset  = 0;

int  lastReminderHour  = -1;
int  lastMidnightDay   = -1;

volatile bool          pomodoroActive  = false;
volatile int           pomodoroPhases  = 0;
volatile unsigned long pomodoroEndTime = 0;
volatile bool          pomodoroBreak   = false;

const unsigned long    POMO_DURATION   = 25UL * 60UL * 1000UL;
const unsigned long    POMO_BREAK_DUR  =  5UL * 60UL * 1000UL;

volatile bool pomoChanged  = false;
volatile bool tasksChanged = false;

// [RESTART-FIX] Safe restart handling - flag + timestamp to avoid RTOS state issues
volatile bool restartRequested = false;
volatile unsigned long restartRequestTime = 0;

volatile bool screensaverActive = false;
int           ssRadius = 5, ssDir = 1;
unsigned long lastSSUpdate = 0, lastDisplayTick = 0;
unsigned long lastPomoWsBc = 0;
unsigned long lastStaleChk = 0;

// [V28] Letzte KI-Antwort zwischenspeichern – wenn der Browser nach einem
// Retry gerade (re-)connected, war er zur Broadcast-Zeit noch nicht da.
// sendStateTo() replayt sie beim WS-Connect, falls frisch genug.
#define AI_RESULT_BUF_MAX  4096
static char*  lastAiReplyBuf = nullptr;  // Dynamically allocated (4096 B only on first AI response)
unsigned long lastAiReplyTime      = 0;
bool          lastAiReplyConsumed  = true;


// [V29-BLOCK1] LED-Controller für RGB Breathing/Blink
class LedController {
  struct {
    uint8_t pin;
    uint16_t period_ms;
    uint32_t start_ms;
    bool breathe;  // vs blink
    uint8_t blink_count;
    uint8_t blink_current;
  } leds[3];
  int led_count = 0;

public:
  void addBreather(uint8_t pin, uint16_t period) {
    if (led_count < 3) {
      leds[led_count].pin = pin;
      leds[led_count].period_ms = period;
      leds[led_count].start_ms = millis();
      leds[led_count].breathe = true;
      pinMode(pin, OUTPUT);
      led_count++;
    }
  }

  void addBlinker(uint8_t pin, uint8_t count) {
    if (led_count < 3) {
      leds[led_count].pin = pin;
      leds[led_count].period_ms = 200;  // 100ms on, 100ms off
      leds[led_count].start_ms = millis();
      leds[led_count].breathe = false;
      leds[led_count].blink_count = count * 2;  // on+off = 2 steps per blink
      leds[led_count].blink_current = 0;
      pinMode(pin, OUTPUT);
      led_count++;
    }
  }

  void update() {
    for (int i = 0; i < led_count; i++) {
      uint32_t now = millis();
      uint32_t elapsed = now - leds[i].start_ms;

      if (leds[i].breathe) {
        // Sine-wave breathing: 0..255 PWM
        uint16_t phase = elapsed % leds[i].period_ms;
        // sin(0..π) -> 0..1, mapped to 0..255
        float ratio = (float)phase / leds[i].period_ms;
        float sine = sin(ratio * 3.14159);
        int brightness = (int)(sine * 255);
        analogWrite(leds[i].pin, constrain(brightness, 0, 255));
      } else {
        // Blinking
        if (leds[i].blink_current < leds[i].blink_count) {
          uint16_t phase = (elapsed % leds[i].period_ms);
          bool on = (phase < 100);  // 100ms on
          digitalWrite(leds[i].pin, on ? HIGH : LOW);
        } else {
          digitalWrite(leds[i].pin, LOW);
        }
      }
    }
  }

  void stopBlink(int idx) {
    if (idx >= 0 && idx < led_count && !leds[idx].breathe) {
      leds[idx].blink_current = leds[idx].blink_count;  // mark as done
    }
  }
};

static LedController ledCtrl;

// [V28] Rotierende Funny-Status-Anzeige während KI arbeitet.
unsigned long lastStatusBcTime     = 0;
int           lastStatusIdx        = -1;
static const char* const FUNNY_STATUS[] = {
  "Ich gruebele darueber nach",
  "Tolle Frage!",
  "Perfektioniere die Antwort",
  "Besuche die Bibliothek",
  "Konsultiere die Fachbuecher",
  "Sortiere meine Gedanken",
  "Setze die Denkmuetze auf",
  "Raeume im Gehirn auf",
  "Suche den besten Pfad",
  "Frage drei weise Eulen",
  "Denke im Quantenraum",
  "Pruefe die Quellen doppelt",
  "Schaue hinter die Kulissen",
  "Pinsle am Feinschliff",
  "Das wird richtig gut!",
  "Blaettere in Enzyklopaedien",
  "Hoere Mozart zur Inspiration",
  "Sortiere die Fakten",
  "Feile am Ausdruck",
  "Knete die Worte zurecht",
  "Schnapp mir einen Kaffee",
  "Lade die grauen Zellen",
  "Poliere die Antwort",
  "Ordne die Argumente",
  "Checke Plausibilitaet",
  "Fege Staub vom Wissen",
  "Webe einen Gedankenfaden",
  "Destilliere die Essenz",
  "Hole tief Luft",
  "Rechne gewissenhaft",
  "Konstruiere die Antwort",
  "Denke drei Ecken weiter",
  "Knacke das Problem",
  "Male das grosse Bild",
  "Verknuepfe Puzzleteile",
  "Fertige einen Entwurf",
  "Uebe den richtigen Ton",
  "Formatiere elegant",
  "Schleife die Pointe",
  "Sortiere Gedankensalat",
  "Pfluecke die beste Idee",
  "Reinige die Logik",
  "Graviere die Antwort",
  "Trinke Kaffee, denke nach",
  "Das wird eine gute Antwort!",
  "Stimme die Harmonie",
  "Brain.exe laeuft",
  "Durchstoebere Neuronen",
  "Schnapp mir Notizen",
  "Waege jede Silbe ab",
  "Schwebe durch Ideen",
  "Zaehle Pixel, denke gross",
  "Ruettle am Wortschatz",
  "Suche die elegante Loesung",
  "Wow, was fuer eine Frage!",
  "Ein Moment goettlicher Ruhe",
  "Krame in alten Buechern",
  "Knote den roten Faden",
  "Schmiede Worte zu Saetzen",
  "Lausche dem Ideenstrom",
  "Recherchiere wie ein Detektiv",
  "Stimme Gehirnzellen ab",
  "Tuschel mit der Muse",
  "Brate die Antwort gar",
  "Baue Satz fuer Satz",
  "Schneide Ueberfluessiges raus",
  "Fast fertig, bleib dran!",
  "Lese zwischen den Zeilen",
  "Sortiere Gedanken nach Farbe",
  "Falte die Antwort zusammen"
};
static const size_t FUNNY_STATUS_N = sizeof(FUNNY_STATUS) / sizeof(FUNNY_STATUS[0]);

// [BLE-REMOVED] BLE variables removed - was using ~30-40 KB heap
// Kept dummy bool for OLED display compatibility (still references "Bluetooth" status)
volatile bool deviceConnected = false;  // Always false - no BLE anymore

// ─── AUFGABEN & HABITS ───────────────────────────────────────
struct Task {
  String        name;
  float         score;
  unsigned long createdAt; 
  bool          staleAlerted;
  // [DEMO-FEATURE] isFakeTask flag: Wird bei /giverandtask auf true gesetzt
  // Ermöglicht /deleterandtask um alle Demo-Tasks zu löschen
  bool          isFakeTask = false;
};
Task         taskList[75];
volatile int taskCount      = 0;
int          tasksDoneToday = 0;

#define MAX_HABITS 6
// [DEMO-FEATURE] Added isFake flag for demo manipulation
struct Habit { 
  char name[20]; 
  bool doneToday; 
  int streak;
  bool isFake = false;  // Demo manipulation flag
};
Habit habits[MAX_HABITS] = {
  {"Morgenroutine", false, 0},
  {"Sport",         false, 0},
  {"Lernen",        false, 0},
  {"Lesen",         false, 0},
  {"Meditation",    false, 0},
  {"Coding",        false, 0}
};

// [DEMO-FEATURE] Telegram State Machine für /editallstreaks Command
enum TelegramEditState { EDIT_IDLE, EDIT_SELECT_HABIT, EDIT_INPUT_VALUE };
volatile TelegramEditState telegramEditState = EDIT_IDLE;
volatile int selectedHabitIndex = -1;

volatile int checkedIn = 1;  // [FIX-GRAY-UI] Auto-eingecheckt beim Boot – kein Lock-Screen beim ersten Laden

// ─── LED & BITMAPS ───────────────────────────────────────────
unsigned long lastLedUpdate = 0;
int           brightness  = 0, fadeAmount = 5;
volatile bool isNewDataFlash = false;
unsigned long flashTimer  = 0;
int           flashRemain = 0;
String        tempCat = "", tempFach = "";
unsigned long tempCatTime = 0;

const unsigned char bitmap_bt[]     PROGMEM = {0x10,0x30,0x58,0x34,0x12,0x12,0x34,0x58,0x30,0x10};
const unsigned char bitmap_search[] PROGMEM = {0x3C,0x42,0x81,0x81,0x81,0x42,0x3C,0xC0};

// ============================================================
//  HTML  –  Assistant Hub Pro UI (Bleibt unangetastet - Design ist perfekt)
// ============================================================
const char htmlPage[] PROGMEM = R"HTMLEND(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<meta name="theme-color" content="#080812">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="AHub Pro">
<link rel="manifest" href="/manifest.json">
<title>Assistant Hub Pro</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#080812;--glass:rgba(15,13,35,0.65);--glass2:rgba(25,20,55,0.5);--bd:rgba(110,80,220,0.22);--txt:#e2e8f0;--dim:#7a85a0;--ac:#7c3aed;--ac2:#a855f7;--ac-bg:rgba(124,58,237,.17);--ac-bg-strong:rgba(124,58,237,.28);--ac-border:rgba(124,58,237,.45);--ac-glow:rgba(124,58,237,.45);--blob1:#4c1d95;--blob2:#2e1065;--r:18px;--ns:6px 6px 14px rgba(0,0,0,.65),-2px -2px 8px rgba(255,255,255,.03);--ni:inset 4px 4px 10px rgba(0,0,0,.65),inset -2px -2px 6px rgba(255,255,255,.03)}
html,body{height:100%;overflow:hidden;font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--txt)}
/* MESH GRADIENT */
.bg{position:fixed;inset:0;z-index:0;overflow:hidden;pointer-events:none}
.blob{position:absolute;border-radius:50%;filter:blur(85px);opacity:.38}
.b1{width:660px;height:660px;background:radial-gradient(var(--blob1),transparent 70%);top:-18%;left:-12%;animation:bm1 22s ease-in-out infinite alternate}
.b2{width:560px;height:560px;background:radial-gradient(var(--blob2),transparent 70%);bottom:-18%;right:-12%;animation:bm2 28s ease-in-out infinite alternate}
.b3{width:460px;height:460px;background:radial-gradient(var(--blob2),transparent 70%);top:35%;left:38%;animation:bm3 19s ease-in-out infinite alternate}
.b4{width:360px;height:360px;background:radial-gradient(var(--blob1),transparent 70%);bottom:14%;left:4%;animation:bm4 24s ease-in-out infinite alternate}
@keyframes bm1{to{transform:translate(190px,140px) scale(1.28)}}
@keyframes bm2{to{transform:translate(-140px,-110px) scale(1.18)}}
@keyframes bm3{to{transform:translate(-95px,-75px) scale(1.45)}}
@keyframes bm4{to{transform:translate(110px,-55px) scale(.82)}}
/* LAYOUT */
.wrap{position:relative;z-index:1;display:grid;grid-template-columns:46px 276px 1fr 354px;gap:9px;padding:9px;height:100vh;max-height:100vh}
.glass{background:var(--glass);backdrop-filter:blur(26px);-webkit-backdrop-filter:blur(26px);border:1px solid var(--bd);border-radius:var(--r)}
/* SIDEBAR */
.sb{display:flex;flex-direction:column;align-items:center;padding:11px 0;gap:8px}
.si{width:38px;height:38px;display:flex;align-items:center;justify-content:center;border-radius:11px;color:var(--dim);cursor:pointer;transition:.15s}
.si:hover{background:var(--ac-bg);color:var(--txt)}
.si svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
/* LEFT PANEL */
.lp{display:flex;flex-direction:column;gap:9px;min-height:0}
/* POMODORO CARD */
.pc{flex:0 0 auto;display:flex;flex-direction:column;align-items:center;padding:16px 14px;gap:12px}
.pr-w{position:relative;width:152px;height:152px}
.pr-svg{width:100%;height:100%;transform:rotate(-90deg)}
.rb{fill:none;stroke:rgba(110,80,220,.14);stroke-width:9}
.rp2{fill:none;stroke:url(#rg);stroke-width:9;stroke-linecap:round;stroke-dasharray:326.73;stroke-dashoffset:326.73;transition:stroke-dashoffset 1s linear}
.pl{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px}
.pt{font-size:1.68rem;font-weight:700;letter-spacing:2px;color:var(--txt)}
.ps{font-size:.64rem;color:var(--dim)}
/* NEUMORPHIC BUTTONS */
.neu{background:#100e22;box-shadow:var(--ns);border:none;border-radius:11px;color:var(--dim);cursor:pointer;padding:9px 14px;font-size:.82rem;font-family:inherit;transition:.13s;display:flex;align-items:center;justify-content:center;gap:6px}
.neu:hover{color:var(--txt);box-shadow:8px 8px 18px rgba(0,0,0,.58),-3px -3px 10px rgba(255,255,255,.04)}
.neu:active{box-shadow:var(--ni);color:var(--ac2)}
.neu svg{width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.neu.w{width:100%}
.neu.rd{color:#ef4444}.neu.rd:active{color:#fca5a5}
.psetup{width:100%;display:flex;flex-direction:column;gap:7px}
.pctrl{display:flex;gap:7px}
label.fl{font-size:.71rem;color:var(--dim)}
input[type=number],select{width:100%;background:rgba(255,255,255,.05);border:1px solid var(--bd);border-radius:9px;padding:8px 10px;color:var(--txt);font-size:.82rem;outline:none;font-family:inherit;color-scheme:dark}
select option{background:#13102b;color:#e2e8f0}
/* HABIT CARD */
.hc{flex:1;display:flex;flex-direction:column;gap:9px;padding:14px;overflow-y:auto;min-height:0}
.sec-h{font-size:.71rem;color:var(--dim);text-transform:uppercase;letter-spacing:.1em;font-weight:600}
.hg{display:grid;grid-template-columns:1fr 1fr;gap:6px}
.hi{padding:9px 10px;border-radius:12px;background:rgba(255,255,255,.03);border:1px solid rgba(255,255,255,.06);cursor:pointer;transition:.18s;display:flex;flex-direction:column;gap:4px}
.hi:hover{border-color:var(--ac-border);transform:translateY(-2px)}
.hi.done{background:var(--ac-bg-strong);border-color:var(--ac-border)}
.hn{font-size:.75rem;color:var(--txt);font-weight:500}
.hst{font-size:.63rem;color:var(--dim)}
.hst.fire{color:#f97316}
/* TASK PANEL */
.tp{display:flex;flex-direction:column;min-height:0;overflow:hidden}
.ph2{display:flex;align-items:center;justify-content:space-between;padding:12px 14px 7px}
.pttl{font-size:.73rem;color:var(--dim);text-transform:uppercase;letter-spacing:.1em;font-weight:600}
.pa{display:flex;gap:6px}
.ib{width:32px;height:32px;background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);border-radius:9px;color:var(--dim);cursor:pointer;display:flex;align-items:center;justify-content:center;transition:.15s}
.ib:hover{background:var(--ac-bg-strong);color:var(--txt);border-color:var(--ac-border)}
.ib svg{width:15px;height:15px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.ntf{padding:10px 12px;display:none;flex-direction:column;gap:7px}
.ntf.show{display:flex}
.tl{flex:1;padding:4px 11px 5px;overflow-y:auto;display:flex;flex-direction:column;gap:5px;min-height:0}
.tr{display:flex;align-items:center;gap:9px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.06);padding:9px 11px;border-radius:11px;cursor:grab;transition:.18s;user-select:none}
.tr:hover{background:var(--ac-bg);border-color:var(--ac-border);transform:translateX(2px)}
.tr.dragging{opacity:.3;border-color:var(--ac-border);cursor:grabbing}
.tr.dov{border-color:var(--ac2);background:var(--ac-bg-strong)}
.tg{color:var(--dim);flex-shrink:0}
.tg svg{width:12px;height:14px;fill:currentColor}
.tn{flex:1;font-size:.84rem;color:var(--txt)}
.tsc{font-size:.72rem;color:var(--dim);background:rgba(255,255,255,.07);padding:3px 8px;border-radius:6px}
.tdel{width:24px;height:24px;display:flex;align-items:center;justify-content:center;color:var(--dim);cursor:pointer;border-radius:6px;transition:.13s;flex-shrink:0;opacity:0}
.tr:hover .tdel{opacity:1}
.tdel:hover{background:rgba(239,68,68,.22);color:#ef4444}
.tdel svg{width:13px;height:13px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.dz{margin:4px 11px 10px;padding:9px;border:2px dashed rgba(239,68,68,.22);border-radius:11px;text-align:center;font-size:.72rem;color:rgba(239,68,68,.42);transition:.18s;flex-shrink:0}
.dz.dov{border-color:rgba(239,68,68,.68);background:rgba(239,68,68,.09);color:rgba(239,68,68,.82)}
/* CHAT PANEL */
.cp{display:flex;flex-direction:column;min-height:0;overflow:hidden}
.chd{display:flex;align-items:center;justify-content:space-between;padding:12px 14px 7px;flex-shrink:0}
.wsd{width:8px;height:8px;border-radius:50%;background:#ef4444;transition:.35s;flex-shrink:0}
.wsd.on{background:#22c55e;box-shadow:0 0 9px #22c55e}
.cm{flex:1;overflow-y:auto;padding:10px 12px;display:flex;flex-direction:column;gap:8px;min-height:0}
.msg{max-width:83%;padding:10px 14px;border-radius:13px;font-size:.86rem;line-height:1.52;word-break:break-word}
.msg.bot{background:var(--ac-bg);border:1px solid var(--ac-border);color:var(--txt);align-self:flex-start;border-bottom-left-radius:4px;white-space:pre-wrap;position:relative;padding-bottom:36px}
/* [FINALFIXES] Copy button: unten-links, echtes Element (nicht ::before) */
.msg-copy-btn{position:absolute;bottom:6px;left:8px;background:0;border:0;cursor:pointer;color:var(--ac2);font-size:1.2em;opacity:.7;transition:.15s;padding:2px 4px;z-index:10}
.msg-copy-btn:hover{opacity:1}
.msg.bot strong{color:var(--ac2);font-weight:700}
.msg.bot em{color:var(--txt);font-style:italic}
.msg.bot code{background:rgba(0,0,0,.28);padding:1px 5px;border-radius:4px;font-family:ui-monospace,Menlo,monospace;font-size:.82em}
.msg.bot h3,.msg.bot h4,.msg.bot h5{margin:6px 0 3px;color:var(--ac);font-weight:700;line-height:1.25}
.msg.bot h3{font-size:1.02em}.msg.bot h4{font-size:.96em}.msg.bot h5{font-size:.9em}
.msg.bot ul{margin:4px 0 4px 14px;padding:0}
.msg.bot li{margin:2px 0}
/* [FINALFIXES] Copy-Button entfernt – wird per JS eingefügt */
.msg.user{background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.12);color:var(--txt);align-self:flex-end;border-bottom-right-radius:4px}
.msg.think{background:transparent;color:var(--dim);font-style:italic;font-size:.79rem;padding:4px 2px;align-self:flex-start}
/* [V30] Unit 4: fade-swap for rotating KI status messages */
.msg.think .stxt{display:inline-block;transition:opacity .28s ease}
.msg.think .stxt.fade{opacity:0}
.cft{display:flex;flex-direction:column;gap:7px;padding:9px 12px 11px;background:rgba(0,0,0,.22);border-top:1px solid rgba(255,255,255,.05);border-radius:0 0 var(--r) var(--r);flex-shrink:0}
.cin-row{display:flex;align-items:center;gap:8px}
.cin{flex:1;background:rgba(255,255,255,.06);border:1px solid var(--ac-border);border-radius:22px;padding:10px 16px;color:var(--txt);font-size:.86rem;outline:none;font-family:inherit}
.cin:focus{border-color:var(--ac);background:var(--ac-bg)}
.cin::placeholder{color:var(--dim)}
.sbtn{width:40px;height:40px;background:linear-gradient(135deg,var(--ac),var(--ac2));border:none;border-radius:50%;color:#fff;cursor:pointer;display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:.15s;box-shadow:0 4px 14px var(--ac-glow)}
.sbtn:hover{transform:scale(1.08);box-shadow:0 6px 20px var(--ac-glow)}
.sbtn:active{transform:scale(.94)}
.sbtn svg{width:16px;height:16px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
/* LOCK SCREEN */
#ls{position:fixed;inset:0;backdrop-filter:blur(24px);background:rgba(8,8,18,.78);display:flex;flex-direction:column;justify-content:center;align-items:center;z-index:9999;gap:18px}
#ls.hidden{display:none}
.lbtn{width:98px;height:98px;border-radius:50%;background:var(--ac-bg-strong);border:2px solid var(--ac-border);display:flex;align-items:center;justify-content:center;cursor:pointer;transition:.22s;box-shadow:0 0 44px var(--ac-glow)}
.lbtn:hover{transform:scale(1.07);box-shadow:0 0 65px var(--ac-glow)}
.lbtn svg{width:44px;height:44px;fill:var(--ac2)}
.ltxt{color:var(--dim);font-size:.93rem;text-align:center;line-height:1.72}
/* UTILS */
::-webkit-scrollbar{width:3px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--ac-border);border-radius:4px}
.hidden{display:none!important}
@keyframes fi{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:translateY(0)}}
.fi{animation:fi .22s ease}
@keyframes pulse{0%,100%{box-shadow:0 0 22px var(--ac-bg)}50%{box-shadow:0 0 54px var(--ac-glow)}}
.pumping{animation:pulse 2.2s ease-in-out infinite}
@media(max-width:860px){.wrap{grid-template-columns:1fr;overflow-y:auto;height:auto;min-height:100vh}.sb{display:none}}
/* [V20] MODE TOGGLE */
.mtog{display:flex;background:#0c0b1c;border-radius:22px;padding:3px;gap:2px;box-shadow:var(--ni);align-self:center}
.mtb{padding:4px 13px;border:none;border-radius:18px;font-size:.71rem;font-family:inherit;cursor:pointer;color:var(--dim);background:transparent;transition:.18s;font-weight:500;white-space:nowrap}
.mtb.on{background:linear-gradient(135deg,var(--ac),var(--ac2));color:#fff;box-shadow:0 2px 8px var(--ac-glow)}
.mhint{font-size:.67rem;color:var(--dim);transition:.18s}
/* [V20] PLAN BUTTON */
@keyframes spin{to{transform:rotate(360deg)}}
.plan-spin svg{animation:spin .9s linear infinite}
.ib.plan{border-color:var(--ac-border)}
.ib.plan:hover{background:var(--ac-bg-strong);border-color:var(--ac);color:var(--ac2)}
.ib.plan:disabled{opacity:.45;cursor:not-allowed;pointer-events:none}
/* [V25] THINKING DOTS */
@keyframes dotB{0%,80%,100%{transform:translateY(0);opacity:.4}40%{transform:translateY(-5px);opacity:1}}
.dots span{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--dim);margin:0 2px;vertical-align:middle;animation:dotB 1.1s ease-in-out infinite}
.dots span:nth-child(2){animation-delay:.18s}.dots span:nth-child(3){animation-delay:.36s}
/* [V25] SCORE BADGE COLORS */
.tsc.urg{color:#f97316;background:rgba(249,115,22,.13);border:1px solid rgba(249,115,22,.25)}
.tsc.mid{color:#eab308;background:rgba(234,179,8,.11);border:1px solid rgba(234,179,8,.22)}
.tsc.lo{color:#22c55e;background:rgba(34,197,94,.1);border:1px solid rgba(34,197,94,.18)}
/* [V25] TOAST */
#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);background:rgba(20,16,45,.92);backdrop-filter:blur(14px);border:1px solid var(--ac-border);color:var(--txt);padding:9px 20px;border-radius:22px;font-size:.81rem;z-index:10000;transition:transform .25s,opacity .25s;opacity:0;pointer-events:none}
#toast.show{transform:translateX(-50%) translateY(0);opacity:1}
/* [V25] HABIT BOUNCE */
@keyframes hBnc{0%,100%{transform:scale(1)}50%{transform:scale(1.09)}}
.hi.bounce{animation:hBnc .22s ease}
/* [V27] ACCENT DROPDOWN – vertikal unter Logo, passt in 46px-Sidebar */
.logo-wrap{position:relative;z-index:60}
.ac-menu{position:absolute;top:46px;left:50%;transform:translateX(-50%);display:flex;flex-direction:column;gap:8px;padding:9px 7px;background:var(--glass);backdrop-filter:blur(26px);-webkit-backdrop-filter:blur(26px);border:1px solid var(--bd);border-radius:12px;box-shadow:var(--ns);z-index:1000;animation:fi .16s ease}
.ac-menu.hidden{display:none}
.acd{width:22px;height:22px;border-radius:50%;cursor:pointer;border:2px solid transparent;transition:.15s;flex-shrink:0}
.acd:hover{transform:scale(1.12);border-color:rgba(255,255,255,.35)}
.acd.active{border-color:var(--txt);transform:scale(1.12)}
</style>
</head>
<body>
<div class="bg">
  <div class="blob b1"></div><div class="blob b2"></div>
  <div class="blob b3"></div><div class="blob b4"></div>
</div>

<div id="ls" class="hidden">
  <div class="lbtn" onclick="doCI()"><svg viewBox="0 0 24 24"><path d="M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"/></svg></div>
  <div class="ltxt">Du bist ausgecheckt.<br>Drücke den Button zum Einchecken.</div>
</div>

<div class="wrap">
  <div class="glass sb">
    <div class="logo-wrap">
      <div class="si" title="Akzentfarbe wählen" tabindex="0" onclick="toggleAc(event)" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();toggleAc(event);}"><svg viewBox="0 0 24 24"><path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/></svg></div>
      <div class="ac-menu hidden" id="acMenu">
        <div class="acd active" style="background:#7c3aed" onclick="setAc('#7c3aed','#a855f7')" title="Lila"></div>
        <div class="acd" style="background:#0d9488" onclick="setAc('#0d9488','#2dd4bf')" title="Teal"></div>
        <div class="acd" style="background:#1d4ed8" onclick="setAc('#1d4ed8','#60a5fa')" title="Blau"></div>
        <div class="acd" style="background:#94a3b8" onclick="setAc('#94a3b8','#e2e8f0')" title="Slate"></div>
        <div class="acd" style="background:#16a34a" onclick="setAc('#16a34a','#4ade80')" title="Grün"></div>
      </div>
    </div>
    <div style="flex:1"></div>
    <div class="si" id="checkoutBtn" title="Auschecken (Telegram-Modus)" onclick="doCheckout()" style="color:var(--ac);"><svg viewBox="0 0 24 24"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg></div>
    <div class="si" id="ramSI" title="RAM frei" style="display:none"><svg viewBox="0 0 24 24"><rect x="2" y="7" width="20" height="14" rx="2"/></svg></div>
  </div>

  <div class="lp">
    <div class="glass pc" id="poC">
      <svg width="0" height="0" style="position:absolute"><defs><linearGradient id="rg" x1="0%" y1="0%" x2="100%" y2="100%"><stop offset="0%" stop-color="#7c3aed"/><stop offset="100%" stop-color="#e879f9"/></linearGradient></defs></svg>
      <div class="pr-w" id="prw">
        <svg class="pr-svg" viewBox="0 0 120 120">
          <circle class="rb" cx="60" cy="60" r="52"/>
          <circle class="rp2" id="pr" cx="60" cy="60" r="52"/>
        </svg>
        <div class="pl"><div class="pt" id="pT">25:00</div><div class="ps" id="pS">Bereit</div></div>
      </div>
      <div class="psetup" id="pSetu">
        <label class="fl">Phasen (á 25 Min)</label>
        <input type="number" id="pp" value="1" min="1" max="4">
        <button class="neu w" onclick="startP()"><svg viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3" fill="currentColor"/></svg>Pomodoro starten</button>
      </div>
      <div class="pctrl hidden" id="pCtrl"><button class="neu rd" onclick="cancelP()"><svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="2"/></svg>Stoppen</button></div>
    </div>

    <div class="glass hc">
      <div class="sec-h">🌱 Habits &amp; Streaks</div>
      <div class="hg" id="hG"><div class="hi" style="grid-column:1/-1;text-align:center;color:var(--dim);font-size:.78rem">Verbinde…</div></div>
    </div>
  </div>

  <div class="glass tp">
    <div class="ph2">
      <span class="pttl">📝 Aufgaben</span>
      <div class="pa">
        <button class="ib plan" id="planBtn" onclick="genPlan()" title="KI-Plan generieren"><svg viewBox="0 0 24 24" stroke-width="1.8"><path d="M9.663 17h4.673M12 3v1m6.364 1.636-.707.707M21 12h-1M4 12H3m3.343-5.657-.707-.707m2.828 9.9a5 5 0 1 1 7.072 0l-.548.547A3.374 3.374 0 0 0 14 18.469V19a2 2 0 1 1-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z"/></svg></button>
        <button class="ib" onclick="togNT()" title="Neue Aufgabe"><svg viewBox="0 0 24 24"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg></button>
        <button class="ib" onclick="wsSend({t:'req',what:'state'})" title="Aktualisieren"><svg viewBox="0 0 24 24"><polyline points="1 4 1 10 7 10"/><path d="M3.51 15a9 9 0 1 0 .49-3.35"/></svg></button>
      </div>
    </div>
    <div class="ntf" id="ntf">
      <select id="cat" onchange="updF()"><option value="todo">To-do</option><option value="hausaufgabe">Hausaufgabe</option><option value="test">Test</option></select>
      <select id="fach"></select>
      <input type="number" id="days" value="1" min="1" max="14" placeholder="Tage bis Abgabe">
      <button class="neu w" onclick="subT()"><svg viewBox="0 0 24 24"><polyline points="20 6 9 17 4 12"/></svg>Hinzufügen</button>
    </div>
    <div class="tl" id="tl"><p id="noT" style="font-size:.82rem;color:var(--dim);padding:6px 4px">Keine Aufgaben 🎉</p></div>
    <div class="dz" id="dz" ondragover="dvDZ(event)" ondragleave="dlDZ()" ondrop="dropDZ(event)">🗑 Hier ablegen zum Löschen</div>
  </div>

  <div class="glass cp">
    <div class="chd">
      <span class="pttl">🤖 KI-Assistent</span>
      <div style="display:flex;align-items:center;gap:7px">
        <div class="wsd" id="wd"></div><span style="font-size:.69rem;color:var(--dim)" id="wst">Verbinde…</span>
      </div>
    </div>
    <div class="cm" id="cm"><div class="msg bot fi">Hallo! Ich bin bereit. Was kann ich für dich tun? 👋</div></div>
    <div class="cft">
      <div style="display:flex;align-items:center;justify-content:space-between">
        <div class="mtog">
          <button class="mtb on" id="modeQ" onclick="setMode('quick')">&#9889; Quick</button>
          <button class="mtb" id="modeD" onclick="setMode('deep')">&#128300; Deep</button>
        </div>
        <span class="mhint" id="modeHint">Schnell &amp; pr&auml;zise</span>
      </div>
      <div class="cin-row">
        <input class="cin" id="ci" placeholder="Frage stellen…" onkeypress="if(event.key==='Enter')sndC()">
        <button class="sbtn" onclick="sndC()"><svg viewBox="0 0 24 24"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2" fill="currentColor"/></svg></button>
      </div>
    </div>
  </div>
</div>

<script>
const WS_URL  = `ws://${location.hostname}:81`;
const CIRC    = 326.73;
const POMO_T  = 25 * 60;
let ws, wsOK = false, thinkEl = null, chatBusy = false;
let tasks = [], dragSrc = null;
let chatMode = 'quick'; // [V20] quick | deep

// [V30] Unit 4: 70 rotating funny German status messages (client-side only).
const kiStatus = [
  "Ich grüble darüber 🤔",
  "Tolle Frage! 🚀",
  "Perfektioniere die Antwort 💎",
  "Besuche die Bibliothek 📚",
  "Konsultiere meine Notizen 📝",
  "Zähle die Schafe... 🐑",
  "Schärfe meinen Bleistift ✏️",
  "Checke mit Einstein ⚛️",
  "Frage meinen Assistenten 🤖",
  "Rechne mit Kaffee ☕",
  "Sortiere Gedanken alphabetisch 🔤",
  "Poliere die Metaphern ✨",
  "Erwäge alle Optionen 🧭",
  "Lese zwischen den Zeilen 👓",
  "Hole tief Luft 🌬️",
  "Stimme meine Synapsen 🎻",
  "Recherchiere gründlich 🔍",
  "Blättere im Lexikon 📖",
  "Rühre im Ideentopf 🥣",
  "Orientiere mich kurz 🗺️",
  "Kalibriere die Logik 🧮",
  "Staube die Fakten ab 🧹",
  "Frage die Eulen 🦉",
  "Teste eine Hypothese 🧪",
  "Dreh eine Runde nachdenken 🌀",
  "Befrage meine Datenbank 💾",
  "Mische die Argumente 🎲",
  "Schaue in die Glaskugel 🔮",
  "Prüfe zwei Gehirnhälften 🧠",
  "Ziehe eine Checkliste 📋",
  "Übersetze aus Gedanken 💭",
  "Warte kurz auf Inspiration 💡",
  "Brainstorme mit mir selbst 🌪️",
  "Überprüfe nochmal die Fakten ✅",
  "Verfasse einen Entwurf 📄",
  "Korrigiere den Entwurf 🖊️",
  "Feile am Schliff 🪚",
  "Trinke virtuellen Espresso ☕",
  "Klettere ins Archiv 🪜",
  "Lausche dem Orakel 👂",
  "Sortiere die Pixel 🧩",
  "Packe die Antwort ein 📦",
  "Verknüpfe die Punkte 🔗",
  "Lüfte die Denkerstirn 🌫️",
  "Schmiede das Argument 🔨",
  "Falte die Gedanken ordentlich 🗂️",
  "Frage mein Unterbewusstes 🌙",
  "Pinsle an den Details 🖌️",
  "Lasse die Zahnräder laufen ⚙️",
  "Suche das passende Wort 🔎",
  "Schüttele den Kopf frei 💆",
  "Wiege Pro und Contra ⚖️",
  "Verdichte die Erkenntnisse 🧱",
  "Hüpfe durch Paragraphen 🦘",
  "Male ein Mindmap 🗯️",
  "Ziehe die Quintessenz 🎯",
  "Buchstabiere rückwärts 🔁",
  "Rolle die Fakten aus 🥟",
  "Schnipsele die Absätze 📑",
  "Drücke auf den Denken-Knopf 🔘",
  "Grabe tiefer in der Frage ⛏️",
  "Zücke den Taschenrechner 🧮",
  "Konferiere mit Sokrates 🏛️",
  "Würfle mit den Nummern 🎲",
  "Lasse die Eulen nicken 🦉",
  "Sortiere Worte nach Gewicht ⚖️",
  "Stöbere im Kopfregal 📚",
  "Schleife die Pointe 💠",
  "Spitze die Konzentration 🎯",
  "Gleich bin ich fertig ⏳"
];

// [V30] Unit 4: rotating status animator for the think bubble.
// Initial fixed sequence, then random cycle every 2500 ms. No repeats.
function startKiStatus(msgElement){
  if(!msgElement) return null;
  const h = { el: msgElement, t1: 0, t2: 0, iv: 0, last: -1 };
  const setTxt = s => {
    const span = h.el.querySelector('.stxt');
    if(!span) return;
    span.classList.add('fade');
    setTimeout(() => { span.textContent = s; span.classList.remove('fade'); }, 180);
  };
  setTxt("Verbinde mit Assistent... 🔗");
  h.t1 = setTimeout(() => setTxt("Schicken der Anfrage... 📤"), 500);
  h.t2 = setTimeout(() => {
    const pick = () => {
      let i = Math.floor(Math.random() * kiStatus.length);
      if(i === h.last) i = (i + 1) % kiStatus.length;
      h.last = i;
      setTxt(kiStatus[i]);
    };
    pick();
    h.iv = setInterval(pick, 2500);
  }, 1500);
  return h;
}
function stopKiStatus(h){
  if(!h) return;
  if(h.t1) clearTimeout(h.t1);
  if(h.t2) clearTimeout(h.t2);
  if(h.iv) clearInterval(h.iv);
  h.t1 = h.t2 = h.iv = 0;
}
let kiHandle = null;

function setMode(m) {
  chatMode = m;
  document.getElementById('modeQ').classList.toggle('on', m === 'quick');
  document.getElementById('modeD').classList.toggle('on', m === 'deep');
  document.getElementById('modeHint').textContent = m === 'quick' ? 'Schnell & präzise' : 'Tiefe Recherche';
}
function genPlan() {
  if (chatBusy) return;
  const btn = document.getElementById('planBtn');
  btn.classList.add('plan-spin'); btn.disabled = true;
  // [V28] Phase 1 wie im Chat – Status wird vom ESP aus live via 'aistatus' aktualisiert.
  // [V30] Unit 4: rotierende lustige Status-Messages starten.
  thinkEl = addThink('Verbinde mit Assistent... 🔗');
  kiHandle = startKiStatus(thinkEl);
  chatBusy = true;
  wsSend({t:'plan'});
}

function initWS() {
  ws = new WebSocket(WS_URL);
  ws.onopen    = () => { wsOK = true;  setDot(true); };
  ws.onclose   = () => { wsOK = false; setDot(false); chatBusy = false; stopKiStatus(kiHandle); kiHandle = null; if (thinkEl) { thinkEl.remove(); thinkEl = null; } setTimeout(initWS, 3000); };
  ws.onerror   = () => { wsOK = false; setDot(false); };
  ws.onmessage = e  => { try { handle(JSON.parse(e.data)); } catch(x) {} };
}
function wsSend(o) { if (wsOK) ws.send(JSON.stringify(o)); }
function setDot(on) {
  document.getElementById('wd').classList.toggle('on', on);
  document.getElementById('wst').textContent = on ? 'Verbunden' : 'Getrennt';
}

function handle(m) {
  switch(m.t) {
    case 'state':
      renderT(m.tasks || []); renderH(m.habits || []); updP(m.pomo);
      if (m.checkedIn === 0) lock();
      if (m.ram) document.getElementById('ramSI').title = 'RAM: ' + m.ram + ' B frei';
      break;
    case 'tasks':  renderT(m.data || []); break;
    case 'habits': renderH(m.data || []); break;
    case 'pomo':   updP(m);               break;
    case 'chat':   onAI(m.text);          break;
    case 'aistatus': updateThink(m.text); break;
    // [CRITICAL-FIX] Add streaming handlers – without these aiDelta never updates UI
    case 'aiDelta': {
      // Incremental SSE chunk – append to current bot message
      if (!thinkEl) return;  // No active AI response
      const stxt = thinkEl.querySelector('.stxt');
      if (stxt && m.c) {
        // Replace dots with actual content as it streams in
        const dots = thinkEl.querySelector('.dots');
        if (dots) dots.remove();
        if (!stxt.nextSibling) {
          const txt = document.createElement('span');
          txt.className = 'ai-stream';
          stxt.appendChild(txt);
        }
        const stream = stxt.querySelector('.ai-stream') || stxt;
        stream.innerHTML += renderMd(m.c);
      }
      break;
    }
    case 'aiDone': {
      // Streaming complete – replace think element with final message
      if (!thinkEl) return;
      const txt = thinkEl.querySelector('.stxt');
      const content = txt ? txt.textContent : '';
      // [FINALFIXES-BUG1] DO NOT set thinkEl=null here – let onAI() handle conversion
      if (content) onAI(content);  // Finalize with renderMd (onAI will clear thinkEl)
      break;
    }
    case 'lock':   m.v ? unlock() : lock(); break;
  }
}

let ciOK = true;
function lock()   { ciOK = false; document.getElementById('ls').classList.remove('hidden'); }
function unlock() { ciOK = true;  document.getElementById('ls').classList.add('hidden'); }
function doCI()   { wsSend({t:'checkin', v:1}); unlock(); }
function doCheckout() {
  if (confirm('Auschecken?\n\nDanach reagiert das System auf Telegram statt WebUI.\nDu kannst dich jederzeit über den Home-Button wieder einchecken.')) {
    wsSend({t:'checkin', v:0});
    lock();
  }
}
function doCheckout() {
  if (confirm('Auschecken? Telegram-Modus wird aktiviert und WebUI gesperrt.')) {
    wsSend({t:'checkin', v:0});
    lock();
  }
}

function updP(p) {
  if (!p) return;
  const t   = p.time || 0;
  const mm  = v => String(Math.floor(v / 60)).padStart(2, '0');
  const ss  = v => String(v % 60).padStart(2, '0');
  document.getElementById('pT').textContent = mm(t) + ':' + ss(t);
  document.getElementById('pS').textContent = p.active ? (p.isBreak ? '☕ Pause' : (p.phases > 1 ? p.phases + ' Phasen übrig' : '🎯 Letzte Phase')) : 'Bereit';
  document.getElementById('pr').style.strokeDashoffset = p.active ? CIRC * (1 - t / (p.isBreak ? 300 : POMO_T)) : CIRC;
  document.getElementById('pSetu').classList.toggle('hidden',  p.active);
  document.getElementById('pCtrl').classList.toggle('hidden', !p.active);
  document.getElementById('prw').classList.toggle('pumping',   p.active && !p.isBreak);
}
function startP()  { wsSend({t:'pomo', action:'start', phases: parseInt(document.getElementById('pp').value)}); }
function cancelP() { wsSend({t:'pomo', action:'cancel'}); }

function renderH(data) {
  const g = document.getElementById('hG'); g.innerHTML = '';
  data.forEach((h, i) => {
    const el = document.createElement('div');
    el.className = 'hi fi' + (h.done ? ' done' : '');
    const fire = h.streak >= 3;
    el.innerHTML = '<div class="hn">' + h.name + '</div><div class="hst' + (fire ? ' fire' : '') + '">' + (fire ? '🔥 ' : '⬜ ') + h.streak + ' Tage</div>';
    el.onclick = () => { el.classList.toggle('done'); el.classList.add('bounce'); el.addEventListener('animationend',()=>el.classList.remove('bounce'),{once:true}); wsSend({t:'habit', action:'toggle', idx: i}); };
    g.appendChild(el);
  });
}

function renderT(data) {
  tasks = data;
  const list = document.getElementById('tl'); list.innerHTML = '';
  if (!data.length) { list.innerHTML = '<p id="noT" style="font-size:.82rem;color:var(--dim);padding:6px 4px">Keine Aufgaben 🎉</p>'; return; }
  data.forEach((t, i) => {
    const row = document.createElement('div');
    row.className = 'tr fi'; row.draggable = true; row.dataset.idx = i;
    const sc=Math.round(t.score), scCls=sc>=8?'tsc urg':sc>=5?'tsc mid':'tsc lo';
    row.innerHTML = '<div class="tg"><svg viewBox="0 0 8 16" fill="currentColor"><circle cx="2" cy="2.5" r="1.2"/><circle cx="6" cy="2.5" r="1.2"/><circle cx="2" cy="8" r="1.2"/><circle cx="6" cy="8" r="1.2"/><circle cx="2" cy="13.5" r="1.2"/><circle cx="6" cy="13.5" r="1.2"/></svg></div><div class="tn">' + (i + 1) + '. ' + t.name + '</div><div class="' + scCls + '">' + sc + '</div><div class="tdel" onclick="delT(' + i + ',event)"><svg viewBox="0 0 24 24"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg></div>';
    row.ondragstart = e => { dragSrc = i; row.classList.add('dragging'); e.dataTransfer.effectAllowed = 'move'; };
    row.ondragend = () => { row.classList.remove('dragging'); document.querySelectorAll('.tr').forEach(r => r.classList.remove('dov')); };
    row.ondragover = e => { e.preventDefault(); document.querySelectorAll('.tr').forEach(r => r.classList.remove('dov')); row.classList.add('dov'); };
    row.ondragleave = () => row.classList.remove('dov');
    row.ondrop = e => { e.preventDefault(); row.classList.remove('dov'); if (dragSrc !== null && dragSrc !== i) movT(dragSrc, i); dragSrc = null; };
    list.appendChild(row);
  });
}

function delT(i, e) { if (e && e.stopPropagation) e.stopPropagation(); tasks.splice(i, 1); renderT(tasks); wsSend({t:'task', action:'del', idx: i}); showToast('🗑 Aufgabe gelöscht'); }
function movT(from, to) { const moved = tasks.splice(from, 1)[0]; tasks.splice(to, 0, moved); renderT(tasks); wsSend({t:'task', action:'reorder', from, to}); }
function dvDZ(e)  { e.preventDefault(); document.getElementById('dz').classList.add('dov'); }
function dlDZ()   { document.getElementById('dz').classList.remove('dov'); }
function dropDZ(e) { e.preventDefault(); dlDZ(); if (dragSrc !== null) { delT(dragSrc, null); dragSrc = null; } }

let ntOpen = false;
function updF() {
  const c = document.getElementById('cat').value;
  document.getElementById('fach').innerHTML = c === 'todo'
    ? '<option value="1.5">Wichtig (Hoch)</option><option value="1.0">Normal</option><option value="0.5">Unwichtig</option>'
    : '<option value="1.5">Mathe</option><option value="1.4">NT</option><option value="1.2">MI</option><option value="1.2">GG/G</option><option value="1.3">Englisch</option><option value="1.3">Franz</option><option value="1.0">Andere</option>';
}
updF();
function togNT() { ntOpen = !ntOpen; document.getElementById('ntf').classList.toggle('show', ntOpen); }
function subT() {
  const c = document.getElementById('cat').value, fw = parseFloat(document.getElementById('fach').value), d = parseInt(document.getElementById('days').value), sl = document.getElementById('fach');
  togNT(); wsSend({t:'task', action:'add', cat:c, weight:fw, days:d, fname:sl.options[sl.selectedIndex].text.split(' ')[0]});
  showToast('✅ Aufgabe hinzugefügt');
}

// [V28] Mini-Markdown: **bold**, *italic*, `code`, ## Heading, - list.
// HTML zuerst escapen, dann gezielte Tags einsetzen – kein fremder HTML-Input möglich.
function renderMd(s){
  if(!s) return '';
  s = s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

  // [CRITICAL-FIX] Tabellen mit Separator-Zeilen korrekt rendern
  // Problem: |---|---| (separator) wurde als Datenzeile behandelt
  // Lösung: Filter Separators und style Header mit Akzentfarbe
  s = s.replace(/(\|[^|\n]+\|.*\n)+/g, m => {
    let rows = m.trim().split('\n').filter(r => r.trim());
    if (rows.length < 2) return m;

    // [CRITICAL-FIX] Remove separator row (|---|---|)
    rows = rows.filter(row => !/^\|\s*[-:]+\s*(\|\s*[-:]+\s*)*\|?\s*$/.test(row));

    if (rows.length < 1) return m;  // Empty after filter

    let html = '<table style="border-collapse:collapse;width:100%;margin:4px 0">';
    rows.forEach((row, i) => {
      const cells = row.split('|').map(c => c.trim()).filter(c => c);
      html += '<tr>';
      cells.forEach(cell => {
        // [CRITICAL-FIX] First row is header (th) with accent color background
        const tag = i === 0 ? 'th' : 'td';
        const style = i === 0
          ? 'border:1px solid var(--ac-border);padding:4px;text-align:left;background:var(--ac);color:#fff;font-weight:bold'
          : 'border:1px solid var(--ac-border);padding:4px;text-align:left';
        html += `<${tag} style="${style}">${cell}</${tag}>`;
      });
      html += '</tr>';
    });
    html += '</table>';
    return html;
  });

  // [V29-BLOCK1] A2: Code-Blöcke (```...```)
  s = s.replace(/```([a-z]*)\n([\s\S]*?)```/g, '<pre style="background:rgba(0,0,0,.28);padding:8px;border-radius:4px;overflow-x:auto;margin:4px 0"><code>$2</code></pre>');

  // [CRITICAL-FIX] Headings mit Akzentfarbe und Bold-Unterstützung
  // [FINAL-FIX] Extended to support #### and ##### headings
  s = s.replace(/^# ([^\n]+)$/gm,'<h3 style="color:var(--ac);font-weight:bold">$1</h3>')
       .replace(/^## ([^\n]+)$/gm,'<h4 style="color:var(--ac);font-weight:bold">$1</h4>')
       .replace(/^### ([^\n]+)$/gm,'<h5 style="color:var(--ac);font-weight:bold">$1</h5>')
       .replace(/^#### ([^\n]+)$/gm,'<h6 style="color:var(--ac);font-weight:bold">$1</h6>')
       .replace(/^##### ([^\n]+)$/gm,'<p style="color:var(--ac);font-weight:bold;font-size:0.9em;margin:4px 0">$1</p>');

  // Inline
  // [MARKDOWN-FIX] Only bold if 2+ characters - prevents **W**ir becoming <b>W</b>ir
  s = s.replace(/\*\*([^*\n]{2,})\*\*/g,'<strong>$1</strong>')
       .replace(/(^|[^*])\*([^*\n]+)\*/g,'$1<em>$2</em>')
       .replace(/`([^`\n]+)`/g,'<code style="background:rgba(0,0,0,.28);padding:1px 5px;border-radius:4px">$1</code>');

  // Lists
  s = s.replace(/(^|\n)(?:- |\u2022 )(.+)/g,'$1<li>$2</li>');
  s = s.replace(/(<li>.*<\/li>\n?)+/g, m => '<ul style="margin:4px 0 4px 14px;padding:0">'+m.replace(/\n/g,'')+'</ul>');

  return s;
}
function copyFallback(text, btn) {
  // Fallback for browsers without Clipboard API (HTTP contexts, older browsers)
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  document.body.appendChild(textarea);
  textarea.select();
  try {
    if (document.execCommand('copy')) {
      showToast('✅ Kopiert!');
    } else {
      showToast('❌ Fehler beim Kopieren');
    }
  } catch (err) {
    showToast('❌ Fehler beim Kopieren');
  } finally {
    document.body.removeChild(textarea);
  }
}

function addM(txt, cls) {
  const c = document.getElementById('cm'), d = document.createElement('div');
  d.className = 'msg ' + cls + ' fi';
  if (cls === 'think' || cls === 'bot') d.innerHTML = cls === 'bot' ? renderMd(txt) : txt;
  else d.textContent = txt;

  // [SOCKET-RESET] Copy-Button mit Fallback für HTTP-Browser
  if (cls === 'bot') {
    // Store original HTML content BEFORE adding button
    const originalHTML = d.innerHTML;

    const btn = document.createElement('button');
    btn.className = 'msg-copy-btn';
    // SVG copy icon (cleaner design)
    btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path></svg>';
    btn.title = 'In Zwischenablage kopieren';

    btn.onclick = (e) => {
      e.stopPropagation();
      const textToCopy = d.textContent || d.innerText || '';
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(textToCopy).catch(() => copyFallback(textToCopy, btn));
      } else {
        copyFallback(textToCopy, btn);
      }
    };
    d.appendChild(btn);
  }

  c.appendChild(d); c.scrollTop = c.scrollHeight; return d;
}
function addThink(label) {
  const dots = '<span class="dots"><span></span><span></span><span></span></span>';
  const lab  = label || '';
  // [V28] .stxt umschliesst den Textteil, damit er per updateThink() lebend getauscht werden kann
  return addM('<span class="stxt">' + lab + '</span>\u2009' + dots, 'think');
}
function updateThink(label) {
  if (!thinkEl) return;
  // [V30] Unit 4: Live 'aistatus' vom Server übersteuert die lustige Rotation.
  stopKiStatus(kiHandle);
  kiHandle = null;
  const s = thinkEl.querySelector('.stxt');
  if (s) {
    s.textContent = label;
    // [FINALFIXES-BUG1] Hide thinking-bubble when label becomes empty
    if (label === '') {
      thinkEl.style.display = 'none';
    }
  }
}
function sndC() {
  if (chatBusy) return;
  const inp = document.getElementById('ci'), txt = inp.value.trim();
  if (!txt) return;
  inp.value = '';
  addM(txt, 'user');
  // [V28] Phase 1: sofortiger lokaler Echo – ESP braucht ~300-500ms für TLS,
  // ohne diesen Echo fühlt sich das Tippen träge an.
  // [V30] Unit 4: rotierende lustige Status-Messages starten.
  thinkEl = addThink('Verbinde mit Assistent... 🔗');
  kiHandle = startKiStatus(thinkEl);
  chatBusy = true;
  wsSend({t:'chat', text:txt, mode:chatMode});
}
function onAI(text) {
  // [V27] Fallback: falls thinkEl nicht (mehr) existiert, frische Bot-Bubble erzeugen,
  // sonst ginge die AI-Antwort verloren (Quick-Mode "toter Text"-Bug).
  // [V30] Unit 4: rotierende Status-Messages stoppen bevor die echte Antwort kommt.
  stopKiStatus(kiHandle);
  kiHandle = null;
  if (thinkEl) { thinkEl.className = 'msg bot fi'; thinkEl.innerHTML = renderMd(text); thinkEl = null; }
  else { addM(text, 'bot'); }
  chatBusy = false;
  const planBtn = document.getElementById('planBtn');
  if (planBtn) { planBtn.classList.remove('plan-spin'); planBtn.disabled = false; }
  document.getElementById('cm').scrollTop = 9999;
}

let _tT;
function showToast(msg){const t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');clearTimeout(_tT);_tT=setTimeout(()=>t.classList.remove('show'),2200);}

function setAc(ac,ac2){
  const r=document.documentElement.style;
  const rgb=ac.slice(1).match(/../g).map(x=>parseInt(x,16)).join(',');
  r.setProperty('--ac',ac); r.setProperty('--ac2',ac2);
  r.setProperty('--bd','rgba('+rgb+',.22)');
  r.setProperty('--ac-bg','rgba('+rgb+',.17)');
  r.setProperty('--ac-bg-strong','rgba('+rgb+',.28)');
  r.setProperty('--ac-border','rgba('+rgb+',.45)');
  r.setProperty('--ac-glow','rgba('+rgb+',.45)');
  r.setProperty('--blob1',ac);
  r.setProperty('--blob2',ac2);
  const stops=document.querySelectorAll('#rg stop');
  if(stops.length===2){stops[0].setAttribute('stop-color',ac);stops[1].setAttribute('stop-color',ac2);}
  document.querySelectorAll('.acd').forEach(d=>d.classList.toggle('active',d.style.background===ac));
  localStorage.setItem('ac',ac); localStorage.setItem('ac2',ac2);
}
function toggleAc(e){if(e)e.stopPropagation();const m=document.getElementById('acMenu');if(m)m.classList.toggle('hidden');}
document.addEventListener('click',e=>{const m=document.getElementById('acMenu');if(m&&!m.classList.contains('hidden')&&!m.contains(e.target)&&!e.target.closest('.logo-wrap'))m.classList.add('hidden');});
document.addEventListener('keydown',e=>{if(e.key==='Escape'){const m=document.getElementById('acMenu');if(m)m.classList.add('hidden');}});
(function(){const a=localStorage.getItem('ac'),b=localStorage.getItem('ac2');if(a&&b)setAc(a,b);})();

if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js').catch(() => {});
setInterval(() => { if (!wsOK) fetch('/status').then(r => r.json()).then(d => { if (d.checkedIn === 0) lock(); }).catch(() => {}); }, 5000);
initWS();
</script>
<div id="toast"></div>
</body>
</html>
)HTMLEND";

// ============================================================
//  PWA MANIFEST
// ============================================================
const char manifestJson[] PROGMEM = R"MANIFEST(
{
  "name": "Assistant Hub Pro",
  "short_name": "AHub Pro",
  "start_url": "/",
  "display": "standalone",
  "background_color": "#080812",
  "theme_color": "#6d28d9",
  "orientation": "portrait-primary",
  "description": "Persoenlicher Produktivitaets-Assistent",
  "icons": [{
    "src": "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ccircle cx='50' cy='50' r='50' fill='%23080812'/%3E%3Cpath d='M50 15 L80 32 L80 68 L50 85 L20 68 L20 32Z' fill='none' stroke='%23a855f7' stroke-width='4'/%3E%3Cline x1='50' y1='50' x2='50' y2='85' stroke='%23a855f7' stroke-width='3'/%3E%3Cline x1='20' y1='32' x2='80' y2='68' stroke='%23a855f7' stroke-width='2' opacity='0.5'/%3E%3C/svg%3E",
    "sizes": "any",
    "type": "image/svg+xml",
    "purpose": "any maskable"
  }]
}
)MANIFEST";

const char swJs[] PROGMEM = R"SWJS(
const CV = 'ahub-pro-v1';
self.addEventListener('install', e => e.waitUntil(caches.open(CV).then(c => c.addAll(['/']))));
self.addEventListener('activate', e => e.waitUntil(caches.keys().then(k => Promise.all(k.filter(n => n !== CV).map(n => caches.delete(n))))));
self.addEventListener('fetch', e => {
  if (e.request.url.includes(':81')) return;
  e.respondWith(fetch(e.request).catch(() => caches.match(e.request)));
});
)SWJS";

// [BLE-REMOVED] BLE Callbacks class removed (MyServerCallbacks)

// ============================================================
//  WEBSOCKET BROADCAST HELPERS
// ============================================================
void bcPomo() {
  long t = pomodoroActive ? max(0L, (long)((pomodoroEndTime - millis()) / 1000)) : 0;
  DynamicJsonDocument doc(256);
  doc["t"] = "pomo";
  doc["active"] = pomodoroActive;
  doc["time"]   = t;
  doc["phases"] = pomodoroPhases;
  doc["isBreak"]= pomodoroBreak;
  String j; serializeJson(doc, j);
  webSocket.broadcastTXT(j.c_str(), j.length());
}

void bcTasks() {
  DynamicJsonDocument doc(3072); // bis zu 75 Tasks mit String-Namen
  doc["t"] = "tasks";
  JsonArray data = doc.createNestedArray("data");
  for (int i = 0; i < taskCount; i++) {
    JsonObject tObj = data.createNestedObject();
    tObj["name"]  = taskList[i].name;
    tObj["score"] = taskList[i].score;
  }
  String j; j.reserve(2000); serializeJson(doc, j);
  webSocket.broadcastTXT(j.c_str(), j.length());
}

void bcHabits() {
  DynamicJsonDocument doc(1024);
  doc["t"] = "habits";
  JsonArray data = doc.createNestedArray("data");
  for (int i = 0; i < MAX_HABITS; i++) {
    JsonObject hObj = data.createNestedObject();
    hObj["name"]   = habits[i].name;
    hObj["done"]   = habits[i].doneToday;
    hObj["streak"] = habits[i].streak;
  }
  String j; serializeJson(doc, j);
  webSocket.broadcastTXT(j.c_str(), j.length());
}

void bcLock(int v) {
  String j = "{\"t\":\"lock\",\"v\":" + String(v) + "}";
  webSocket.broadcastTXT(j.c_str(), j.length());
}

// [V28] Status-Broadcast für 3-Phasen-Anzeige während KI-Anfragen:
// Phase 1 (lokal, Browser): "Verbinde mit Assistent"
// Phase 2 (ESP, sofort bei Job-Annahme): "Schicke deine Frage..."
// Phase 3 (ESP, alle 2.5s): rotierende Funny-Status-Texte.
void bcStatus(const String& text) {
  DynamicJsonDocument doc(256);
  doc["t"] = "aistatus";
  doc["text"] = text;
  String j; serializeJson(doc, j);
  webSocket.broadcastTXT(j.c_str(), j.length());
}

void bcChat(const String& text) {
  // [REFACTOR-V31] Cache in char buffer for late-connecting clients
  // Allocate buffer on first use (4096 B)
  if (!lastAiReplyBuf) {
    lastAiReplyBuf = (char*)malloc(AI_RESULT_BUF_MAX);
  }
  if (lastAiReplyBuf) {
    strncpy(lastAiReplyBuf, text.c_str(), AI_RESULT_BUF_MAX - 1);
    lastAiReplyBuf[AI_RESULT_BUF_MAX - 1] = '\0';
  }
  lastAiReplyTime = millis();
  lastAiReplyConsumed = false;


  DynamicJsonDocument doc(4096);
  doc["t"] = "chat";
  doc["text"] = text;
  String j; serializeJson(doc, j);
  webSocket.broadcastTXT(j.c_str(), j.length());
}

void sendStateTo(uint8_t num) {
  DynamicJsonDocument doc(3072);
  doc["t"] = "state";
  doc["checkedIn"] = checkedIn;
  doc["ram"] = ESP.getFreeHeap();
  
  JsonObject pomo = doc.createNestedObject("pomo");
  pomo["active"] = pomodoroActive;
  pomo["time"]   = pomodoroActive ? max(0L, (long)((pomodoroEndTime - millis()) / 1000)) : 0;
  pomo["phases"] = pomodoroPhases;
  pomo["isBreak"]= pomodoroBreak;
  
  JsonArray tasks = doc.createNestedArray("tasks");
  for (int i = 0; i < taskCount; i++) {
    JsonObject tObj = tasks.createNestedObject();
    tObj["name"]  = taskList[i].name;
    tObj["score"] = taskList[i].score;
  }
  
  JsonArray habs = doc.createNestedArray("habits");
  for (int i = 0; i < MAX_HABITS; i++) {
    JsonObject hObj = habs.createNestedObject();
    hObj["name"]   = habits[i].name;
    hObj["done"]   = habits[i].doneToday;
    hObj["streak"] = habits[i].streak;
  }
  
  String j; j.reserve(2500); serializeJson(doc, j);
  webSocket.sendTXT(num, j.c_str(), j.length());

  // [REFACTOR-V31] Replay cached response if client was offline at broadcast
  // [FIX-NULL] lastAiReplyBuf kann nullptr sein – erst auf != nullptr prüfen!
  if (!lastAiReplyConsumed && lastAiReplyBuf && lastAiReplyBuf[0] &&
      (millis() - lastAiReplyTime) < 60000) {
    DynamicJsonDocument cdoc(4096);
    cdoc["t"] = "chat";
    cdoc["text"] = lastAiReplyBuf;
    cdoc["replay"] = true;
    String cj; serializeJson(cdoc, cj);
    webSocket.sendTXT(num, cj.c_str(), cj.length());
    lastAiReplyConsumed = true;
  }
}

// ============================================================
//  WEBSOCKET EVENT HANDLER (Zero-Copy Parsing)
// ============================================================
void handleWSMsg(uint8_t num, uint8_t* payload, size_t length) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return; // Silent Fail bei ungültigen Daten

  const char* tp = doc["t"];
  if (!tp) return;
  String type = String(tp);

  if (type == "checkin") {
    int v = doc["v"] | 0;
    int prevState = checkedIn;
    checkedIn = v;
    
    // [ENGINEER-FIX5] Heap-aware mode switching on check-in/check-out transitions
    if (v == 1 && prevState == 0) {
      // CHECK-IN: User wakes up the device - WebUI mode active
      wakeUpDisplay();
      Serial.printf("[CHECKIN] Mode: WebUI active (Heap: %u B, MaxAlloc: %u B)\n",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // Force-free lastAiReplyBuf to maximize heap for upcoming AI requests
      if (lastAiReplyBuf) {
        free(lastAiReplyBuf);
        lastAiReplyBuf = nullptr;
        Serial.println("[CHECKIN] Freed lastAiReplyBuf for fresh session");
      }
    } else if (v == 0 && prevState == 1) {
      // CHECK-OUT: User leaves - Telegram mode active
      Serial.printf("[CHECKOUT] Mode: Telegram active (Heap: %u B, MaxAlloc: %u B)\n",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // Force-free lastAiReplyBuf - WebUI replay no longer needed
      if (lastAiReplyBuf) {
        free(lastAiReplyBuf);
        lastAiReplyBuf = nullptr;
        Serial.println("[CHECKOUT] Freed lastAiReplyBuf - heap returned to Telegram pool");
      }
      // Reset TLS health state to ensure clean Telegram polling
      tlsHealthy = false;
    }
    bcLock(v);
  }
  else if (type == "pomo") {
    if (checkedIn == 0) { webSocket.sendTXT(num, "{\"t\":\"lock\",\"v\":0}"); return; }
    String action = doc["action"] | "";
    if (action == "start") {
      pomodoroPhases  = constrain((int)(doc["phases"] | 1), 1, 4);
      pomodoroActive  = true;
      pomodoroBreak   = false;
      pomodoroEndTime = millis() + POMO_DURATION;
      wakeUpDisplay();
    } else if (action == "cancel") {
      pomodoroActive = false;
      pomodoroPhases = 0;
      pomodoroBreak  = false;
      wakeUpDisplay();
    }
    bcPomo();
  }
  else if (type == "task") {
    if (checkedIn == 0) return;
    String action = doc["action"] | "";
    if (action == "add") {
      processTask(doc["cat"] | "todo", doc["weight"] | 1.0f, doc["days"] | 1, doc["fname"] | "Task");
      // triggerEventBlink();  // [LITE] LED deaktiviert – einkommentieren für LED-Support
    }
    else if (action == "del") {
      int idx = doc["idx"] | -1;
      if (idx >= 0 && idx < taskCount) {
        for (int j = idx; j < taskCount - 1; j++) taskList[j] = taskList[j + 1];
        taskCount--; tasksDoneToday++;
        updateDisplay();
        bcTasks();
      }
    }
    else if (action == "reorder") {
      int from = doc["from"] | -1;
      int to   = doc["to"]   | -1;
      if (from >= 0 && from < taskCount && to >= 0 && to < taskCount && from != to) {
        Task moved = taskList[from];
        if (from < to) for (int j = from; j < to; j++) taskList[j] = taskList[j + 1];
        else           for (int j = from; j > to; j--) taskList[j] = taskList[j - 1];
        taskList[to] = moved;
        bcTasks();
      }
    }
  }
  else if (type == "habit") {
    if (checkedIn == 0) return;
    int idx = doc["idx"] | -1;
    if (idx >= 0 && idx < MAX_HABITS) {
      habits[idx].doneToday = !habits[idx].doneToday;
      bcHabits();
      saveHabits();  // [DEMO-FEATURE] Persist habit changes to SPIFFS
      // triggerEventBlink();  // [LITE] LED deaktiviert – einkommentieren für LED-Support
    }
  }
  else if (type == "chat") {
    if (checkedIn == 0) return;
    String text = doc["text"] | "";
    if (text.length() == 0) return;

    // [DEEP-FIX] Mode-aware mit Payload-Guard
    const char* modeStr = doc["mode"] | "quick";
    bool isDeep  = (strcmp(modeStr, "deep") == 0);
    const char* model = isDeep ? "llama-3.3-70b-versatile" : "llama-3.1-8b-instant";

    String prompt;
    if (isDeep) {
      // [FINALFIXES] Input-Limit erhöht: 600 → 2000 Zeichen (keine Abschnitte mehr)
      String userText = text;
      if (userText.length() > 2000) {
        Serial.printf("[CHAT] Text zu lang (%d > 2000), kürze für deep.\n", userText.length());
        userText = userText.substring(0, 1997) + "...";
      }
      // Deep prompt komprimiert (HTTP 413 Fix)
      prompt = "Recherche strukturiert (## Ueberschriften). Max 450 Woerter, schliesse mit ## Fazit. Deutsch. Thema: " + userText;
    } else {
      // [FINALFIXES] Quick Input-Limit: 400 → 1000 Zeichen
      String userText = text;
      if (userText.length() > 1000) userText = userText.substring(0, 997) + "...";
      prompt = "Kurz (max 120 W), freundlich, Deutsch. **Fett** fuer Keys. Frage: " + userText;
    }

    Serial.printf("[CHAT] %s-Mode, Prompt-Größe: %d Zeichen\n", isDeep ? "Deep" : "Quick", prompt.length());

    if (!requestAI(AI_JOB_WEB, prompt, model)) {
      // [FIX-UX] requestAI() sendet bei Heap-Fehler bereits eine eigene bcChat()-Meldung.
      // "ausgelastet" nur anzeigen wenn wirklich ein anderer Job läuft (pending==true).
      if (aiJob.pending) {
        bcChat("⚠️ Die KI ist gerade beschäftigt. Bitte warte kurz.");
      }
    }
  }
  else if (type == "plan") {
    // [DEEP-FIX] Plan-Generator: optimiert für HTTP 413 Prevention
    if (checkedIn == 0) return;
    if (taskCount == 0) { bcChat("ℹ️ Keine offenen Aufgaben – nichts zu planen! 🎉"); return; }

    // Fix 1: Task-Limiter – max 15 wichtigste Tasks
    int taskShow = min((int)taskCount, 15);

    String pp;
    pp.reserve(2000);
    // [V29] Plan-Prompt komprimiert (HTTP 413 Fix)
    pp = "Plan (HH:MM **Aufgabe**, keine Erklaerung, max 250 W, Deutsch):\n\nAufgaben:\n";

    // Fix 2: JSON-Stripping – nur name + score, keine Metadaten
    for (int i = 0; i < taskShow; i++) {
      pp += String(i+1) + ". " + taskList[i].name + " (Prio:" + String((int)taskList[i].score) + ")\n";
      // Fix 3: Früh brechen wenn payload zu groß wird (> 6000 Zeichen)
      if (pp.length() > 6000) {
        pp += "...(+weitere Aufgaben)\n";
        break;
      }
    }

    pp += "\nHabits:\n";
    for (int i = 0; i < MAX_HABITS; i++) {
      if (!habits[i].doneToday) pp += "- " + String(habits[i].name) + "\n";
      if (pp.length() > 6000) break;
    }

    // Fix 3: String-Buffer Guard – kürze aggressiv wenn zu groß
    if (pp.length() > 6000) {
      Serial.printf("[PLAN] Payload zu groß (%d > 6000), kürze Tasks aggressiv.\n", pp.length());
      pp = "Plan (Top 5, HH:MM **Aufgabe**, max 200 W, Deutsch):\n";
      for (int i = 0; i < min(5, taskShow); i++) {
        pp += String(i+1) + ". " + taskList[i].name + " (Prio:" + String((int)taskList[i].score) + ")\n";
      }
    }

    Serial.printf("[PLAN] Payload-Größe: %d Zeichen\n", pp.length());
    if (!requestAI(AI_JOB_WEB, pp, "llama-3.3-70b-versatile")) {
      if (aiJob.pending) {
        bcChat("⚠️ KI ausgelastet. Versuche es gleich nochmal.");
      }
    }
  }
  else if (type == "req") {
    String what = doc["what"] | "";
    if (what == "state") sendStateTo(num);
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED: break;
    case WStype_CONNECTED:    sendStateTo(num); break;
    case WStype_TEXT:         handleWSMsg(num, payload, length); break; // [FIX] Zero-Copy
    default: break;
  }
}

// ============================================================
//  TELEGRAM HELPERS (Sicheres JSON Routing)
// ============================================================
int sendTgMsg(const String& chat_id, const String& text) {
  WiFiClientSecure lc; lc.setInsecure();
  HTTPClient http;
  http.begin(lc, "https://api.telegram.org/bot" + String(BOTtoken) + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection",   "close");
  http.setTimeout(8000);

  // [FIX] Sicheres JSON bauen – 4096 nötig: AI-Antworten können >2000 Zeichen sein
  DynamicJsonDocument req(4096);
  req["chat_id"] = chat_id;
  req["text"]    = text;
  String payload; payload.reserve(3000); serializeJson(req, payload);

  int code  = http.POST(payload);
  int msgId = -1;
  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, http.getString());
    msgId = doc["result"]["message_id"] | -1;
  }
  http.end(); lc.stop();
  return msgId;
}

int sendTgMsgHtml(const String& chat_id, const String& html) {
  WiFiClientSecure lc; lc.setInsecure();
  HTTPClient http;
  http.begin(lc, "https://api.telegram.org/bot" + String(BOTtoken) + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection",   "close");
  http.setTimeout(8000);
  DynamicJsonDocument req(1024);
  req["chat_id"]    = chat_id;
  req["text"]       = html;
  req["parse_mode"] = "HTML";
  String payload; payload.reserve(900); serializeJson(req, payload);
  int code = http.POST(payload);
  int msgId = -1;
  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, http.getString());
    msgId = doc["result"]["message_id"] | -1;
  }
  http.end(); lc.stop();
  return msgId;
}

void editTgMsg(const String& chat_id, int msg_id, const String& new_text) {
  WiFiClientSecure lc; lc.setInsecure();
  HTTPClient http;
  http.begin(lc, "https://api.telegram.org/bot" + String(BOTtoken) + "/editMessageText");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection",   "close");
  http.setTimeout(8000);

  DynamicJsonDocument req(4096); // AI-Antworten können >2000 Zeichen sein
  req["chat_id"]    = chat_id;
  req["message_id"] = msg_id;
  req["text"]       = new_text;
  String payload; payload.reserve(3000); serializeJson(req, payload);

  http.POST(payload);
  http.end(); lc.stop();
}

void sendTgMenu(const String& chat_id, const String& text) {
  WiFiClientSecure lc; lc.setInsecure();
  HTTPClient http;
  http.begin(lc, "https://api.telegram.org/bot" + String(BOTtoken) + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection",   "close");
  http.setTimeout(8000);

  DynamicJsonDocument req(2048);
  req["chat_id"] = chat_id;
  req["text"]    = text;
  
  JsonObject reply_markup = req.createNestedObject("reply_markup");
  JsonArray keyboard = reply_markup.createNestedArray("keyboard");
  JsonArray row1 = keyboard.createNestedArray(); row1.add("-Aufgaben-"); row1.add("⏱ Pomodoro");
  JsonArray row2 = keyboard.createNestedArray(); row2.add("KI-Frage"); row2.add("📊 Status");
  JsonArray row3 = keyboard.createNestedArray(); row3.add("Erledigt"); row3.add("➕ Neu");
  
  reply_markup["resize_keyboard"] = true;
  reply_markup["persistent"]      = true;
  
  String payload; serializeJson(req, payload);
  http.POST(payload);
  http.end(); lc.stop();
}

// ============================================================
//  AI-TASK (Core 1)
// ============================================================

// [LOW-HEAP-UX] Free optional buffers and yield to let the allocator consolidate.
// Returns bytes freed. Called by aiTask when a job was accepted under low heap.
uint32_t aggressiveHeapCleanup() {
  uint32_t freed = 0;
  Serial.println("[TIER1-CLEANUP] Starting aggressive heap cleanup...");

  // Step 1: Free the late-join replay cache (4 KB) – safe to discard when heap is low
  if (lastAiReplyBuf) {
    free(lastAiReplyBuf);
    lastAiReplyBuf = nullptr;
    freed += AI_RESULT_BUF_MAX;
    Serial.printf("[TIER1] Freed lastAiReplyBuf (%.1f KB)\n", AI_RESULT_BUF_MAX / 1024.0);
  }

  // Step 2: Clear any pending strings (accumulated String objects in stack)
  // Force a garbage collection pass by yielding repeatedly
  Serial.println("[TIER1] Forcing garbage collection with yield loops...");
  for (int i = 0; i < 5; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t heapNow = ESP.getFreeHeap();
    Serial.printf("[TIER1] Yield %d: Heap now %u B\n", i + 1, heapNow);
  }

  // Step 3: Snapshot heap state before and after explicit yield
  uint32_t heapBefore = ESP.getFreeHeap();
  uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
  Serial.printf("[TIER1] Before extended yield: Free=%u B, MaxAlloc=%u B\n",
                heapBefore, maxAllocBefore);

  // Step 4: Extended yield for allocator to coalesce adjacent free blocks
  // This is critical for preventing fragmentation (maxAlloc collapse)
  for (int i = 0; i < 10; i++) {
    vTaskDelay(pdMS_TO_TICKS(50));  // Total 500ms of yielding
  }

  uint32_t heapAfter = ESP.getFreeHeap();
  uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
  uint32_t heapRecovered = (heapAfter > heapBefore) ? (heapAfter - heapBefore) : 0;
  freed += heapRecovered;

  Serial.printf("[TIER1] After extended yield: Free=%u B (+%u B), MaxAlloc=%u B (+%d B)\n",
                heapAfter, heapRecovered, maxAllocAfter,
                (int)maxAllocAfter - (int)maxAllocBefore);

  Serial.printf("[TIER1-CLEANUP] Done: Total freed %u B (%.1f KB)\n", freed, freed / 1024.0);
  return freed;
}

// [BULLETPROOF-TIER2] Enter critical TLS mode – pause other services to maximize heap
void enterCriticalTlsMode(int currentType) {
  if (tlsCriticalMode) return;  // Already active
  tlsCriticalMode = true;
  telegramTaskEnabled = false;
  webSocketTaskEnabled = false;
  networkTaskEnabled = false;
  vTaskDelay(pdMS_TO_TICKS(200));  // Allow tasks to pause gracefully
  Serial.println("[TIER2-CRITICAL] Critical TLS mode ACTIVATED – services paused");
  Serial.printf("[TIER2-CRITICAL] Heap: %u B, MaxAlloc: %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (currentType == AI_JOB_WEB) {
    bcStatus("⏳ Kritischer Modus: RAM wird maximiert...");
  }
}

// [BULLETPROOF-TIER2] Exit critical TLS mode – resume other services
void exitCriticalTlsMode() {
  if (!tlsCriticalMode) return;  // Not active
  tlsCriticalMode = false;
  telegramTaskEnabled = true;
  webSocketTaskEnabled = true;
  networkTaskEnabled = true;
  vTaskDelay(pdMS_TO_TICKS(100));  // Brief pause for task scheduler
  Serial.println("[TIER2-CRITICAL] Critical TLS mode DEACTIVATED – services resumed");
  Serial.printf("[TIER2-CRITICAL] Final heap: %u B, MaxAlloc: %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// [BIGUPDATE] Pre-request memory and heap preparation
void prepareForAiRequest() {
  // Clear any stale payload to prevent fragmentation
  String tempPayload;
  tempPayload.clear();

  // [ENGINEER-FIX2] Memory leak fix: Always free lastAiReplyBuf before AI request
  // Old code only freed when heap < 35KB - this caused buildup over long uptime.
  // New: Force free on EVERY request. Buffer is only 4KB - cheap to re-allocate.
  if (lastAiReplyBuf) {
    Serial.printf("[AI-PREP] Force-freeing lastAiReplyBuf (Heap before: %u B)\n",
                  ESP.getFreeHeap());
    free(lastAiReplyBuf);
    lastAiReplyBuf = nullptr;
    vTaskDelay(pdMS_TO_TICKS(20));  // Brief yield for heap consolidation
  }

  // [SOFT-WIFI] Adjust timeouts/tokens based on WiFi signal strength
  int rssi = WiFi.RSSI();
  uint32_t tlsTimeout = 30000;
  
  if (rssi >= -65) {
    // Excellent - Normal timeout
    bcStatus("📶 Ausgezeichnet");
  } else if (rssi >= -75) {
    // Good but weak - Warn user
    bcStatus("📶 WiFi schwach - wartet...");
    tlsTimeout = 45000;
  } else if (rssi >= -85) {
    // Weak - Slow mode
    bcStatus("📶 Sehr schwaches WiFi...");
    tlsTimeout = 60000;
  } else {
    // Very weak - Last chance (no hard reject!)
    bcStatus("📶 Extrem schwaches WiFi...");
    tlsTimeout = 90000;
  }
  
  persistentAiClient.setTimeout(tlsTimeout);
  Serial.printf("[AI-PREP] RSSI: %d dBm → Timeout: %u ms\n", rssi, tlsTimeout);
  Serial.printf("[AI-PREP] Heap check: %u B free, MaxAlloc: %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// ───────────────────────────────────────────────────────────
// KI MANAGER (requestAI) - Nimmt Anfragen entgegen
// ───────────────────────────────────────────────────────────
bool requestAI(int type, const String& prompt, const String& model, int telMsgId) {
  if (xSemaphoreTake(aiJobMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

  if (aiJob.pending) {
    xSemaphoreGive(aiJobMutex);
    return false;
  }

  // Phase 1: Heap-Cleanup – free old reply buffer (replay window expired)
  if (lastAiReplyBuf && (millis() - lastAiReplyTime > 60000)) {
    free(lastAiReplyBuf);
    lastAiReplyBuf = nullptr;
    Serial.println("[AI] Freed expired lastAiReplyBuf (4 KB recovered).");
  }

  // Phase 2: Heap-Limits
  // [NO-HARD-REJECT] Statt Hard-Reject: Quick-Cleanup VOR der Prüfung + Accept als heapWasLow.
  // HEAP_HARD_MIN = 8000 (physisch absolutes Minimum – bei <8 KB kann selbst WebSocket nicht senden).
  // 8–28 KB: Quick-Cleanup → Accept mit heapWasLow → aiTask führt aggressiveHeapCleanup() aus.
  // 28–55 KB: Accept mit heapWasLow-Flag (aiTask prüft und wartet bei Bedarf).
  const uint32_t HEAP_HARD_MIN = 8000;   // Nur absolutes physisches Minimum
  uint32_t heapNow = ESP.getFreeHeap();
  bool jobHeapWasLow = false;

  // [QUICK-CLEANUP-BEFORE-CHECK] lastAiReplyBuf (4 KB) sofort freigeben wenn Heap knapp.
  // Verhindert unnötige Rejects: 17 KB + 4 KB = 21 KB → Cleanup-Pfad statt Reject.
  if (heapNow < 55000 && lastAiReplyBuf) {
    free(lastAiReplyBuf);
    lastAiReplyBuf = nullptr;
    vTaskDelay(pdMS_TO_TICKS(50));   // kurz yielden damit der Allocator konsolidieren kann
    heapNow = ESP.getFreeHeap();
    Serial.printf("[AI] Quick-Cleanup: lastAiReplyBuf freigegeben, Heap jetzt %u B\n", heapNow);
  }

  if (heapNow < HEAP_HARD_MIN) {
    // Absolutes Minimum (<8 KB) – physisch unmöglich, selbst ein WS-Frame scheitert
    Serial.printf("[AI] Heap absolut kritisch (%u B < %u B) – absoluter Hard-Reject.\n",
                  heapNow, HEAP_HARD_MIN);
    xSemaphoreGive(aiJobMutex);
    // Statisches JSON – kein DynamicJsonDocument (würde bei <8 KB ebenfalls malloc-failen)
    if (type == AI_JOB_WEB) {
      static const char critErrJson[] =
        "{\"t\":\"chat\",\"text\":\"\u26a0\ufe0f RAM < 8 KB. Bitte ESP neu starten.\"}";
      webSocket.broadcastTXT(critErrJson, strlen(critErrJson));
    } else if (type == AI_JOB_TELEGRAM && telMsgId > 0) {
      // Kein TLS-Aufruf riskieren – Nachricht bleibt auf "Ueberlege..."
      Serial.println("[AI] Telegram-Nachricht bleibt stehen (RAM zu niedrig fuer editTgMsg).");
    }
    return false;

  } else if (heapNow < 28000) {
    // Niedrig aber wiederherstellbar: Accept + Cleanup-Flag (aiTask wartet bis 28 KB)
    Serial.printf("[AI] Heap niedrig (%u B) – Accept mit heapWasLow (aiTask macht Cleanup).\n", heapNow);
    jobHeapWasLow = true;
  } else if (heapNow < 55000) {
    // Moderat niedrig: Accept mit Monitoring-Flag
    Serial.printf("[AI] Heap moderat (%u B) – accepting with cleanup flag.\n", heapNow);
    jobHeapWasLow = true;
  }

  // [OPTION3] Check for degraded mode (heap fragmentation)
  uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  bool degradedModeNeeded = false;

  if (heapNow >= HEAP_HARD_MIN && (heapNow < 30000 || maxAllocNow < 38000)) {
    // Heap too fragmented for normal TLS – offer degraded mode
    Serial.printf("[AI-DEGRADE] Low maxAlloc (%u B) – applying degraded mode\n", maxAllocNow);
    degradedModeNeeded = true;

    // Fallback 1: Try smaller model if using 70b
    if (strstr(model.c_str(), "70b")) {
      Serial.println("[AI-DEGRADE] Switching from 70b to 8b model for degraded mode");
    } else {
      // Fallback 2: Will reduce tokens in aiTask
      Serial.println("[AI-DEGRADE] Will reduce tokens for degraded mode");
    }
  }

  // [FIX2] aiJob.result is now static – no malloc needed
  aiJob.result[0] = '\0';  // Clear buffer

  strncpy(aiJob.prompt, prompt.c_str(), AI_PROMPT_MAX - 1);
  aiJob.prompt[AI_PROMPT_MAX - 1] = '\0';

  // [OPTION3] Apply degraded mode if needed
  String actualModel = model;
  if (degradedModeNeeded && strstr(model.c_str(), "70b")) {
    actualModel = "llama-3.1-8b-instant";
    Serial.println("[AI-DEGRADE] Model switched to 8b for degraded mode");
  }

  strncpy(aiJob.model, actualModel.c_str(), AI_MODEL_MAX - 1);
  aiJob.model[AI_MODEL_MAX - 1] = '\0';

  aiJob.type       = type;
  aiJob.telMsgId   = telMsgId;
  aiJob.pending    = true;
  aiJob.done       = false;
  aiJob.heapWasLow = jobHeapWasLow;  // [LOW-HEAP-UX]
  aiJob.degradedMode = degradedModeNeeded;  // [OPTION3]
  // If model was NOT changed but we're in degraded mode, reduce tokens
  aiJob.tokenReductionApplied = (degradedModeNeeded && actualModel == model);
  aiJob.result[0]  = '\0';

  xSemaphoreGive(aiJobMutex);

  wakeUpDisplay();
  if (AITask) xTaskNotifyGive(AITask);

  // ACK an Browser: RAM-Optimierungsstatus ODER normaler Status
  if (type == AI_JOB_WEB) {
    if (jobHeapWasLow) {
      bcStatus("⏳ RAM wird optimiert, die Anfrage kann laenger dauern");
    } else {
      bcStatus("Schicke deine Frage an den Assistenten");
    }
  }
  lastStatusBcTime = millis();
  lastStatusIdx = -1;
  return true;
}
// ───────────────────────────────────────────────────────────
// [DEMO-FEATURE] Habit Persistierung via SPIFFS
// ───────────────────────────────────────────────────────────

// Speichert alle Habits in SPIFFS
void saveHabits() {
  File f = SPIFFS.open("/habits.json", "w");
  if (!f) {
    Serial.println("[HABIT-SAVE] ❌ SPIFFS open failed");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.createNestedArray("habits");
  
  for (int i = 0; i < MAX_HABITS; i++) {
    JsonObject h = arr.createNestedObject();
    h["streak"] = habits[i].streak;
    h["lastDone"] = habits[i].doneToday ? millis() : 0;
    h["isFake"] = habits[i].isFake;
  }
  
  serializeJson(doc, f);
  f.close();
  
  Serial.printf("[HABIT-SAVE] ✅ Saved %d habits to SPIFFS\n", MAX_HABITS);
}

// Lädt alle Habits aus SPIFFS
void loadHabits() {
  File f = SPIFFS.open("/habits.json", "r");
  if (!f) {
    Serial.println("[HABIT-LOAD] No habits.json found (first boot or corrupted)");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, f);
  if (err) {
    Serial.printf("[HABIT-LOAD] ❌ JSON parse error: %s\n", err.c_str());
    f.close();
    return;
  }
  
  JsonArray arr = doc["habits"];
  if (!arr) {
    Serial.println("[HABIT-LOAD] ❌ No habits array in JSON");
    f.close();
    return;
  }
  
  for (int i = 0; i < arr.size() && i < MAX_HABITS; i++) {
    habits[i].streak = arr[i]["streak"] | 0;
    habits[i].doneToday = (arr[i]["lastDone"] | 0) > 0;
    habits[i].isFake = arr[i]["isFake"] | false;
  }
  
  f.close();
  Serial.printf("[HABIT-LOAD] ✅ Loaded %d habits from SPIFFS\n", MAX_HABITS);
}

// ───────────────────────────────────────────────────────────
// [SOCKET-RESET] Persistent TLS Management
// ───────────────────────────────────────────────────────────
void setupPersistentTls() {
  persistentAiClient.setInsecure();
  persistentAiClient.setHandshakeTimeout(30);
  persistentAiClient.setTimeout(30000);

  if (persistentAiClient.connect("api.groq.com", 443, 30000)) {
    tlsHealthy = true;
    Serial.println("[SOCKET-RESET] Persistent TLS initialized");
  } else {
    Serial.println("[SOCKET-RESET] Persistent TLS init failed");
  }
}

// [ENGINEER-FIX4] Track consecutive TLS failures - force full re-init after 2 fails
static uint8_t tlsConsecutiveFailures = 0;

bool ensurePersistentTlsHealth() {
  // Quick check: if recently healthy and still connected → no reconnect needed
  if (tlsHealthy && persistentAiClient.connected()) {
    tlsConsecutiveFailures = 0;  // Reset on success
    return true;
  }

  // [ENGINEER-FIX4] After 2 consecutive failures, force FULL re-init (not just reconnect)
  // This handles undefined-state cases where mbedTLS context is corrupted
  if (tlsConsecutiveFailures >= 2) {
    Serial.println("[ENGINEER-FIX4] 2x consecutive TLS failures - forcing full re-init");
    persistentAiClient.stop();
    vTaskDelay(pdMS_TO_TICKS(500));  // Longer wait for full mbedTLS cleanup
    persistentAiClient = WiFiClientSecure();  // Reconstruct client object
    persistentAiClient.setInsecure();
    persistentAiClient.setHandshakeTimeout(30);
    persistentAiClient.setTimeout(30000);
    tlsConsecutiveFailures = 0;
    Serial.printf("[ENGINEER-FIX4] Re-init complete, Heap: %u B, MaxAlloc: %u B\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  } else {
    // Socket is dead → stop and attempt single reconnect
    persistentAiClient.stop();
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  if (persistentAiClient.connect("api.groq.com", 443, 30000)) {
    tlsHealthy = true;
    tlsConsecutiveFailures = 0;
    Serial.println("[SOCKET-RESET] TLS health restored");
    return true;
  }

  tlsHealthy = false;
  tlsConsecutiveFailures++;  // Increment failure counter
  Serial.printf("[SOCKET-RESET] TLS health check failed (consecutive: %d)\n", tlsConsecutiveFailures);
  return false;
}

// ───────────────────────────────────────────────────────────
// [HOPEFULLYFINAL] warmUpTls() removed – was causing extended TLS handshake blocks
// that triggered TG1WDT hardware watchdog resets on subsequent calls

// ───────────────────────────────────────────────────────────
// KI WORKER (aiTask) - Führt die API-Anfragen aus
// ───────────────────────────────────────────────────────────
void aiTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(100));  // Small delay to ensure task is fully initialized

  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    char currentPrompt[AI_PROMPT_MAX];
    char currentModel[AI_MODEL_MAX];
    int currentType;
    int currentTelId;
    bool currentHeapWasLow;               // [LOW-HEAP-UX]
    bool currentDegradedMode;             // [OPTION3]
    bool currentTokenReductionApplied;    // [OPTION3]

    xSemaphoreTake(aiJobMutex, portMAX_DELAY);
    strncpy(currentPrompt, aiJob.prompt, AI_PROMPT_MAX - 1);
    currentPrompt[AI_PROMPT_MAX - 1] = '\0';
    strncpy(currentModel, aiJob.model, AI_MODEL_MAX - 1);
    currentModel[AI_MODEL_MAX - 1] = '\0';
    currentType       = aiJob.type;
    currentTelId      = aiJob.telMsgId;
    currentHeapWasLow = aiJob.heapWasLow;  // [LOW-HEAP-UX]
    currentDegradedMode = aiJob.degradedMode;  // [OPTION3]
    currentTokenReductionApplied = aiJob.tokenReductionApplied;  // [OPTION3]
    xSemaphoreGive(aiJobMutex);

    Serial.printf("[AI] Start: %s | MaxHeap: %d B | RSSI: %d dBm\n",
                  currentModel, ESP.getMaxAllocHeap(), WiFi.RSSI());

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[AI] Kein WLAN – Request abgebrochen.");
      xSemaphoreTake(aiJobMutex, portMAX_DELAY);
      snprintf(aiJob.result, AI_RESULT_MAX, "⚠️ Kein WLAN. Bitte warte kurz.");
      aiJob.done    = true;
      aiJob.pending = false;
      xSemaphoreGive(aiJobMutex);
      continue;
    }

    // ═══ HEAP GATE ═══ Wait for heap recovery
    // [LOW-HEAP-UX] HEAP_SOFT_MIN lowered from 50000 → 28000 (TLS needs ~25-30 KB).
    // Two paths: low-heap path runs aggressiveHeapCleanup() + 30s wait with status updates;
    // normal path runs 5s wait (unchanged logic, just lower SOFT_MIN constant).
    {
      const uint32_t HEAP_SOFT_MIN = 28000;
      bool heapAbort = false;

      if (currentHeapWasLow) {
        // [LOW-HEAP-UX] Accepted under low heap – run aggressive cleanup first
        Serial.printf("[AI] heapWasLow=true, heap=%u B – running aggressiveHeapCleanup.\n",
                      ESP.getFreeHeap());

        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          display.clearDisplay();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(0, 0);
          display.println("RAM Cleanup...");
          display.printf("Heap: %u B\n", ESP.getFreeHeap());
          display.display();
          xSemaphoreGive(displayMutex);
        }

        aggressiveHeapCleanup();

        // [ENGINEER-FIX3] Extended wait: 30s → 10s to prevent TG1WDT risk
        // BLE-removal makes heap recovery faster, no need for 30s timeout
        // vTaskDelay(500) every iteration keeps task watchdog happy
        const uint32_t WAIT_MAX_MS = 10000;  // Reduced from 30000 to prevent watchdog
        uint32_t waitStart = millis();
        uint32_t lastStatusUpdate = 0;

        while (ESP.getFreeHeap() < HEAP_SOFT_MIN) {
          uint32_t elapsed = millis() - waitStart;
          if (elapsed > WAIT_MAX_MS) {
            Serial.printf("[AI] Heap cleanup timeout after 10s (%u B < %u B) – aborting.\n",
                          ESP.getFreeHeap(), HEAP_SOFT_MIN);
            xSemaphoreTake(aiJobMutex, portMAX_DELAY);
            snprintf(aiJob.result, AI_RESULT_MAX,
                     "RAM Cleanup fehlgeschlagen (nur %u B frei). Bitte ESP neu starten.",
                     ESP.getFreeHeap());
            aiJob.done    = true;
            aiJob.pending = false;
            xSemaphoreGive(aiJobMutex);
            heapAbort = true;
            break;
          }
          // Live status update to browser every 2s
          if (millis() - lastStatusUpdate > 2000) {
            if (currentType == AI_JOB_WEB) {
              char buf[80];
              snprintf(buf, sizeof(buf),
                       "⏳ RAM wird optimiert... %u KB frei", ESP.getFreeHeap() / 1024);
              bcStatus(String(buf));
            }
            if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
              display.clearDisplay();
              display.setTextSize(1);
              display.setTextColor(SSD1306_WHITE);
              display.setCursor(0, 0);
              display.println("RAM Cleanup...");
              display.printf("Heap: %u B\n", ESP.getFreeHeap());
              display.printf("Warte... %us\n", (unsigned)(elapsed / 1000));
              display.display();
              xSemaphoreGive(displayMutex);
            }
            lastStatusUpdate = millis();
          }
          vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!heapAbort && currentType == AI_JOB_WEB) {
          bcStatus("✅ RAM optimiert! Verbinde...");
        }

      } else {
        // Normal path: 5s wait (HEAP_SOFT_MIN now 28000 instead of 50000)
        const uint32_t WAIT_MAX_MS = 5000;
        uint32_t waitStart = millis();
        bool heapWasLowLocal = (ESP.getFreeHeap() < HEAP_SOFT_MIN);

        while (ESP.getFreeHeap() < HEAP_SOFT_MIN) {
          if (millis() - waitStart > WAIT_MAX_MS) {
            uint32_t heapAtTimeout = ESP.getFreeHeap();
            Serial.printf("[AI] Heap recovery timeout (%u B < %u B) – aborting.\n",
                          heapAtTimeout, HEAP_SOFT_MIN);
            xSemaphoreTake(aiJobMutex, portMAX_DELAY);
            snprintf(aiJob.result, AI_RESULT_MAX,
                     "⚠️ Speicher-Freigabe gescheitert (nur %u B). Bitte ESP neu starten.",
                     heapAtTimeout);
            aiJob.done    = true;
            aiJob.pending = false;
            xSemaphoreGive(aiJobMutex);
            heapAbort = true;
            break;
          }

          if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 0);
            display.println("KI laeuft...");
            display.println("Speicher wird");
            display.println("freigegeben...");
            display.println();
            display.printf("Heap: %u B\n", ESP.getFreeHeap());
            display.display();
            xSemaphoreGive(displayMutex);
          }
          vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!heapAbort && heapWasLowLocal) {
          if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 0);
            display.println("Verbinde...");
            display.display();
            xSemaphoreGive(displayMutex);
          }
          updateDisplay();
        }
      }

      if (heapAbort) continue;
    }

    // [HEAP-PROFILE] Profiling snapshot: PRE-TLS state
    {
      uint32_t heapFree = ESP.getFreeHeap();
      uint32_t maxAlloc = ESP.getMaxAllocHeap();
      uint32_t fragments = (heapFree > maxAlloc) ? (heapFree - maxAlloc) : 0;
      Serial.printf("[HEAP-PROFILE] PRE-TLS: free=%u B, maxAlloc=%u B, fragments=%u B (%.1f%% fragmented)\n",
                    heapFree, maxAlloc, fragments, (100.0 * fragments / heapFree));
    }

    // ═══ TIER 2 CRITICAL MODE CHECK ═══ Detect need for service blocking
    // If MaxAllocHeap too fragmented, enter critical mode to maximize contiguous heap
    {
      uint32_t maxAllocBeforeTls = ESP.getMaxAllocHeap();
      const uint32_t TIER2_THRESHOLD = 38000;  // 38 KB – minimum for safe TLS

      Serial.printf("[TLS-PRE] MaxAllocHeap: %u B (threshold: %u B)\n",
                    maxAllocBeforeTls, TIER2_THRESHOLD);

      if (maxAllocBeforeTls < TIER2_THRESHOLD) {
        Serial.println("[TIER2] MaxAllocHeap fragmented! Entering critical mode...");

        // [TIER2] Pause all background services
        enterCriticalTlsMode(currentType);

        // [TIER1] Run enhanced cleanup in critical mode
        uint32_t freedInCritical = aggressiveHeapCleanup();

        // [TIER2] Monitor MaxAllocHeap recovery
        uint32_t maxAllocAfterCleanup = ESP.getMaxAllocHeap();
        Serial.printf("[TIER2] After cleanup: MaxAlloc=%u B (freed ~%u B)\n",
                      maxAllocAfterCleanup, freedInCritical);

        if (maxAllocAfterCleanup < TIER2_THRESHOLD) {
          // [TIER3] Still critical – provide diagnostics before TLS attempt
          Serial.println("[TIER3-DIAGNOSTIC] SEVERE FRAGMENTATION – Detailed analysis:");
          Serial.printf("  Free heap: %u B\n", ESP.getFreeHeap());
          Serial.printf("  Max contiguous: %u B (need: ~50000 B for TLS+POST)\n", maxAllocAfterCleanup);
          Serial.printf("  WiFi RSSI: %d dBm\n", WiFi.RSSI());
          Serial.printf("  WiFi status: %d\n", WiFi.status());

          // [TIER3-WAIT] Konsolidierungswartezeit statt direktem TLS-Versuch.
          // Verhindert "esp-sha: Failed to allocate buf memory" wenn maxAlloc < 38 KB:
          // mbedTLS SHA braucht ~8 KB zusammenhängenden Block – Warten gibt dem Allocator
          // Zeit freie Fragmente zusammenzuführen (bis zu 10 s, Check alle 500 ms).
          if (currentType == AI_JOB_WEB) {
            bcStatus("⏳ RAM stark fragmentiert – warte auf Konsolidierung...");
          }
          for (int _w = 0; _w < 20 && ESP.getMaxAllocHeap() < TIER2_THRESHOLD; _w++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (_w % 4 == 0) {
              Serial.printf("[TIER3] Konsolidierung %d/20: maxAlloc=%u B (Ziel: %u B)\n",
                            _w + 1, ESP.getMaxAllocHeap(), TIER2_THRESHOLD);
            }
          }
          Serial.printf("[TIER2] Weiter mit TLS: maxAlloc=%u B\n", ESP.getMaxAllocHeap());
        } else {
          Serial.printf("[TIER2] Recovery successful! MaxAlloc now %u B\n", maxAllocAfterCleanup);
          if (currentType == AI_JOB_WEB) {
            bcStatus("✅ RAM optimiert! Verbinde zum API...");
          }
        }
      }
    }

    // [SOCKET-RESET] Use persistent TLS instead of fresh sslClient per request
    // This avoids per-request fragmentation that was causing HTTP -1 after 5 requests
    if (!ensurePersistentTlsHealth()) {
      Serial.println("[AI-TLS] Persistent TLS health check failed – aborting");
      // [FIX-TIER2-LEAK] exitCriticalTlsMode() MUSS hier aufgerufen werden!
      // Ohne diesen Call bleibt tlsCriticalMode=true, webSocketTaskEnabled=false
      // und telegramTaskEnabled=false → Gray UI + keine Telegram-Antworten (dauerhaft).
      if (tlsCriticalMode) exitCriticalTlsMode();
      xSemaphoreTake(aiJobMutex, portMAX_DELAY);
      snprintf(aiJob.result, AI_RESULT_MAX, "⚠️ TLS Verbindung fehlgeschlagen.");
      aiJob.done    = true;
      aiJob.pending = false;
      xSemaphoreGive(aiJobMutex);
      if (currentType == AI_JOB_WEB) {
        bcChat("⚠️ TLS Fehler");
      }
      continue;
    }

    // Check WiFi signal strength before attempting to use TLS
    int rssi = WiFi.RSSI();
    if (rssi < -75) {
      Serial.printf("[AI] WiFi signal too weak (RSSI %d dBm) – aborting.\n", rssi);
      if (tlsCriticalMode) exitCriticalTlsMode();  // [TIER2] Exit critical mode on abort
      xSemaphoreTake(aiJobMutex, portMAX_DELAY);
      snprintf(aiJob.result, AI_RESULT_MAX,
               "⚠️ WiFi-Signal zu schwach (RSSI %d dBm). Bitte näher zum Router gehen.",
               rssi);
      aiJob.done = true;
      aiJob.pending = false;
      xSemaphoreGive(aiJobMutex);
      continue;  // Back to while(1) start
    }

    // [SOCKET-RESET] TLS already established via persistent socket
    // No need for TLS retry loop – HTTP POST handles retries below
    Serial.printf("[AI-HTTP] TLS ready, proceeding to HTTP POST (heap: %u B)\n",
                  ESP.getFreeHeap());

    // [PHASE3-PERSISTENT-TLS] TLS is now ready – proceed with HTTP
    if (tlsCriticalMode) {
      Serial.println("[TIER2] Using persistent TLS in critical mode");
    }

    // [BIGUPDATE-POST] HTTP POST with -1 retry logic
    uint32_t t_start = millis();

    // Call preparation function
    prepareForAiRequest();

    // [HEAP-PROFILE] Profiling snapshot: PRE-HTTP state
    {
      uint32_t heapFree = ESP.getFreeHeap();
      uint32_t maxAlloc = ESP.getMaxAllocHeap();
      Serial.printf("[HEAP-PROFILE] PRE-HTTP: free=%u B, maxAlloc=%u B\n",
                    heapFree, maxAlloc);
    }

    const int MAX_HTTP_ATTEMPTS = 2;  // Initial + 1 retry
    int httpCode = 0;
    bool httpSuccess = false;

    for (int httpAttempt = 1; httpAttempt <= MAX_HTTP_ATTEMPTS && !httpSuccess; httpAttempt++) {
      // [BIGUPDATE] Validate heap before each POST attempt
      uint32_t heapBeforePost = ESP.getFreeHeap();
      if (heapBeforePost < 32000) {
        Serial.printf("[AI-POST] Heap low before POST attempt %d: %u B\n",
                      httpAttempt, heapBeforePost);
        // Flush optional cache to recover heap
        if (lastAiReplyBuf) {
          free(lastAiReplyBuf);
          lastAiReplyBuf = nullptr;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }

      // [BIGUPDATE] Build request fresh each attempt
      String bodyLocal;
      {
        DynamicJsonDocument reqDoc(2048);
        reqDoc["model"]       = currentModel;
        reqDoc["stream"]      = false;
        
        // [FINAL-FIX] Dynamic token calculation - prevents truncated AI responses
        // Groq limit: ~7500 tokens combined (input + output)
        int promptTokens = (strlen(currentPrompt) / 4) + 50;
        int maxTokens = (strstr(currentModel, "70b") ? 400 : 500);
        
        // Adjust based on prompt length to maximize output space
        if (promptTokens < 100) {
          maxTokens = min(1200, 7500 - promptTokens);
        } else if (promptTokens < 200) {
          maxTokens = min(1000, 7500 - promptTokens);
        } else if (promptTokens < 400) {
          maxTokens = min(800, 7500 - promptTokens);
        } else {
          maxTokens = min(600, 7500 - promptTokens);
        }
        
        // [OPTION3] Apply token reduction if heap was low
        if (currentTokenReductionApplied) {
          maxTokens = min(300, maxTokens);
          Serial.printf("[AI] Max tokens reduced to %d (degraded mode)\n", maxTokens);
        }
        
        Serial.printf("[AI-TOKEN] Prompt: %d tok, max output: %d tok\n", promptTokens, maxTokens);
        
        reqDoc["max_tokens"]  = maxTokens;
        reqDoc["temperature"] = 0.7;

        JsonArray msgs = reqDoc.createNestedArray("messages");
        JsonObject msg = msgs.createNestedObject();
        msg["role"]    = "user";
        msg["content"] = currentPrompt;

        serializeJson(reqDoc, bodyLocal);
      }

      // Validate request size
      if (bodyLocal.length() > 4096) {
        Serial.printf("[AI] Request too large: %d bytes\n", bodyLocal.length());
        xSemaphoreTake(aiJobMutex, portMAX_DELAY);
        snprintf(aiJob.result, AI_RESULT_MAX, "KI Fehler: Anfrage zu lang (>4KB).");
        aiJob.done = true; aiJob.pending = false;
        xSemaphoreGive(aiJobMutex);
        httpSuccess = true;  // Exit loop, this error won't retry
        break;
      }

      String key = (currentType == AI_JOB_WEB) ? GROQ_API_KEY_WEB : GROQ_API_KEY_TG;

      // [SOCKET-RESET] Use persistent socket for HTTP POST
      HTTPClient httpAttempt_client;
      if (!httpAttempt_client.begin(persistentAiClient, "https://api.groq.com/openai/v1/chat/completions")) {
        Serial.println("[AI-POST] http.begin() failed");
        xSemaphoreTake(aiJobMutex, portMAX_DELAY);
        snprintf(aiJob.result, AI_RESULT_MAX, "⚠️ HTTP init failed");
        aiJob.done = true; aiJob.pending = false;
        xSemaphoreGive(aiJobMutex);
        httpSuccess = true;  // Won't retry init failure
        break;
      }

      httpAttempt_client.addHeader("Content-Type", "application/json");
      httpAttempt_client.addHeader("Authorization", "Bearer " + key);
      httpAttempt_client.addHeader("Connection", "close");
      httpAttempt_client.addHeader("Content-Length", String(bodyLocal.length()));
      httpAttempt_client.setTimeout(35000);  // [BIGUPDATE] 35s for full POST + response

      Serial.printf("[AI-POST] Attempt %d/%d: Body=%d B, Heap=%u B\n",
                    httpAttempt, MAX_HTTP_ATTEMPTS, bodyLocal.length(), ESP.getFreeHeap());

      // [BIGUPDATE] Explicit timeout tracking
      uint32_t postStart = millis();
      httpCode = httpAttempt_client.POST((uint8_t*)bodyLocal.c_str(), bodyLocal.length());
      uint32_t postDuration = millis() - postStart;

      Serial.printf("[AI-POST] Attempt %d returned code %d (duration: %u ms)\n",
                    httpAttempt, httpCode, postDuration);

      // [HEAP-PROFILE] Profiling snapshot: POST-HTTP state
      {
        uint32_t heapFree = ESP.getFreeHeap();
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        Serial.printf("[HEAP-PROFILE] POST-HTTP (attempt %d): free=%u B, maxAlloc=%u B, code=%d\n",
                      httpAttempt, heapFree, maxAlloc, httpCode);
      }

      if (httpCode == 200) {
        // SUCCESS PATH
        String response = httpAttempt_client.getString();
        httpAttempt_client.end();

        Serial.printf("[AI-POST] Response received: %d bytes\n", response.length());

        StaticJsonDocument<2048> responseDoc;
        DeserializationError err = deserializeJson(responseDoc, response);

        if (err) {
          Serial.printf("[AI] JSON parse error: %s\n", err.c_str());
          xSemaphoreTake(aiJobMutex, portMAX_DELAY);
          snprintf(aiJob.result, AI_RESULT_MAX, "KI Fehler: Ungültige Antwort.");
          aiJob.done = true; aiJob.pending = false;
          xSemaphoreGive(aiJobMutex);
        } else {
          const char* content = responseDoc["choices"][0]["message"]["content"];
          if (content) {
            xSemaphoreTake(aiJobMutex, portMAX_DELAY);
            strncpy(aiJob.result, content, AI_RESULT_MAX - 1);
            aiJob.result[AI_RESULT_MAX - 1] = '\0';
            aiJob.done = true;
            aiJob.pending = false;
            xSemaphoreGive(aiJobMutex);

            // [FINALFIXES-BUG3-DIAG] Log response delivery info
            if (currentType == AI_JOB_WEB) {
              Serial.printf("[AI-STORE] Web response stored (%d bytes), broadcasting aiDone\n", (int)strlen(content));

              StaticJsonDocument<128> doneFrame;
              doneFrame["t"] = "aiDone";
              doneFrame["tokens"] = responseDoc["usage"]["completion_tokens"] | 0;
              String doneJson;
              serializeJson(doneFrame, doneJson);
              webSocket.broadcastTXT(doneJson.c_str(), doneJson.length());
            }
            else if (currentType == AI_JOB_TELEGRAM) {
              Serial.printf("[AI-STORE] Telegram response stored (%d bytes), telMsgId=%d, will deliver via networkTask\n",
                            (int)strlen(content), currentTelId);
            }

            Serial.printf("[AI] Success after %u ms\n", postDuration);
          } else {
            Serial.println("[AI] No content in response");
            xSemaphoreTake(aiJobMutex, portMAX_DELAY);
            snprintf(aiJob.result, AI_RESULT_MAX, "KI Fehler: Leere Antwort.");
            aiJob.done = true; aiJob.pending = false;
            xSemaphoreGive(aiJobMutex);
          }
        }

        // [SOCKET-RESET] Increment request counter and check for periodic reset
        requestCountSinceReset++;
        if (requestCountSinceReset >= RESET_SOCKET_AFTER) {
          Serial.printf("[SOCKET-RESET] Counter reached %d – resetting socket\n", RESET_SOCKET_AFTER);
          persistentAiClient.stop();
          vTaskDelay(pdMS_TO_TICKS(500));
          if (persistentAiClient.connect("api.groq.com", 443, 30000)) {
            tlsHealthy = true;
            Serial.println("[SOCKET-RESET] Socket freshly reconnected");
          } else {
            tlsHealthy = false;
            Serial.println("[SOCKET-RESET] Socket reconnect failed!");
          }
          requestCountSinceReset = 0;
        }

        httpSuccess = true;  // Exit loop

      } else if (httpCode == -1 && httpAttempt < MAX_HTTP_ATTEMPTS) {
        // [PHASE3] RETRY PATH for -1 (no TLS reconnect needed – persistent connection handles it)
        httpAttempt_client.end();

        Serial.printf("[AI-POST] HTTP -1 on attempt %d, will retry after 7.5s (persistent TLS stable)\n", httpAttempt);
        xSemaphoreTake(aiJobMutex, portMAX_DELAY);
        snprintf(aiJob.result, AI_RESULT_MAX,
                 "⏳ Verbindung unstabil, versuche erneut... (Versuch %d/2)",
                 httpAttempt);
        aiJob.done = false;
        aiJob.pending = true;
        xSemaphoreGive(aiJobMutex);

        // Broadcast interim status to web
        if (currentType == AI_JOB_WEB) {
          char statusBuf[60];
          snprintf(statusBuf, sizeof(statusBuf),
                   "⏳ Verbindung unstabil... Versuch %d/2", httpAttempt);
          bcStatus(statusBuf);
        }

        vTaskDelay(pdMS_TO_TICKS(7500));  // 7.5s before retry

        // [PHASE3] Persistent TLS remains connected – no reconnect needed!
        // Just retry the POST with the existing stable connection

      } else {
        // ERROR PATH (non -1 or final attempt)
        httpAttempt_client.end();
        // [PHASE3] No sslClient.stop() – persistent TLS remains connected

        // Classify error
        if (httpCode == 401) {
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "⚠️ API-Authentifizierung fehlgeschlagen. Prüfe API-Key.");
        } else if (httpCode == 429) {
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "⚠️ Rate-Limit erreicht. Bitte später versuchen.");
        } else if (httpCode == 413) {
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "KI Fehler: Anfrage zu lang (413).");
        } else if (httpCode == -1) {
          // [BIGUPDATE] Final -1 after retry
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "⚠️ Verbindung fehlgeschlagen nach 2 Versuchen. Heap: %u B. Bitte erneut versuchen.",
                   ESP.getFreeHeap());
          Serial.println("[AI-POST] HTTP -1 after retry – network issue persists");
        } else if (httpCode > 0) {
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "KI Fehler (HTTP %d).", httpCode);
        } else {
          snprintf(aiJob.result, AI_RESULT_MAX,
                   "⚠️ Netzwerkfehler (Code %d).", httpCode);
        }

        xSemaphoreTake(aiJobMutex, portMAX_DELAY);
        aiJob.done = true;
        aiJob.pending = false;
        xSemaphoreGive(aiJobMutex);
        httpSuccess = true;  // Exit loop
      }
    }

    // [PHASE3] Persistent TLS connection remains open for next request
    // No sslClient.stop() needed – connection is maintained across requests
    // This eliminates 36-40KB per-request fragmentation!

    Serial.printf("[AI-REQUEST-COMPLETE] Persistent TLS ready for next request (heap: %u B, maxAlloc: %u B)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // [TIER2] Exit critical TLS mode – resume background services
    if (tlsCriticalMode) {
      exitCriticalTlsMode();
    }

    // [CRITICAL-FIX] Do NOT free aiJob.result here!
    // The main loop reads aiJob.result AFTER aiJob.done=true.
    // Freeing here (outside mutex) causes NULL pointer dereference (LoadProhibited crash).
    // The main loop now frees the buffer safely inside the mutex after reading.

  }
}
// ============================================================
//  DISPLAY & REMINDERS
// ============================================================

// [STARTUP-UI] Display Willkommenstext centered for 3 seconds
void showStartupScreen() {
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println("Willkommen");
  display.setCursor(25, 40);
  display.println("YOUR_NAME");  // ← Deinen Namen hier eintragen
  display.display();

  xSemaphoreGive(displayMutex);

  // Show for 3 seconds
  unsigned long startTime = millis();
  while (millis() - startTime < 3000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void wakeUpDisplay() {
  lastInteractionTime = millis();
  if (screensaverActive) { screensaverActive = false; updateDisplay(); }
}

// [V29-BLOCK1] Trigger blue LED blink auf Events
void triggerEventBlink() {
  ledCtrl.addBlinker(LED_BLUE, 3);  // Blink 3 times
}

void updateDisplay() {
  if (screensaverActive) return;
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  // [BLE-REMOVED] BLE bitmap removed, show WiFi RSSI in corner instead
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    display.print("Wi");
    if (rssi > -60) display.print(":***");
    else if (rssi > -75) display.print(":** ");
    else display.print(":*  ");
  } else {
    display.print("WiFi--");
  }

  if (pomodoroActive) {
    long t = max(0L, (long)((pomodoroEndTime - millis()) / 1000));
    char buf[7];
    sprintf(buf, "%02d:%02d", (int)(t / 60), (int)(t % 60));
    display.setCursor(43, 0); display.print(pomodoroBreak ? "BRK" : buf);
  }

  struct tm ti;
  if (getLocalTime(&ti)) {
    char tb[6]; strftime(tb, sizeof(tb), "%H:%M", &ti);
    display.setCursor(95, 0); display.print(tb);
  }

  display.drawLine(0, 11, 128, 11, SSD1306_WHITE);

  int taskShow = min((int)taskCount, 3);
  for (int i = 0; i < taskShow; i++) {
    int y = 15 + i * 16;
    display.setCursor(0, y);  display.print(taskList[i].name.substring(0, 12));
    display.setCursor(90, y); display.print((int)taskList[i].score);
  }
  if (taskCount == 0) { display.setCursor(0, 16); display.print("Keine Aufgaben"); }

  for (int i = 0; i < MAX_HABITS; i++) {
    display.drawPixel(118 + (i % 3) * 4, 56 + (i / 3) * 4, habits[i].doneToday ? SSD1306_WHITE : SSD1306_BLACK);
    if (!habits[i].doneToday) display.drawRect(118 + (i % 3) * 4, 56 + (i / 3) * 4, 2, 2, SSD1306_WHITE);
  }

  // [V29-BLOCK1] A11: OLED-Live KI-Anzeige – Spinner + erste Zeichen der Antwort
  if (aiJob.pending) {
    static uint32_t spinnerTime = 0;
    static const char* spinners[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    static int spinnerIdx = 0;
    uint32_t now = millis();

    if (now - spinnerTime >= 100) {
      spinnerIdx = (spinnerIdx + 1) % 10;
      spinnerTime = now;
    }

    display.setCursor(43, 55);
    display.print("KI");
    display.print(spinners[spinnerIdx]);

  }

  display.display();
  xSemaphoreGive(displayMutex);
}

void checkTimeReminders() {
  struct tm ti; if (!getLocalTime(&ti)) return;
  int h = ti.tm_hour, m = ti.tm_min;
  if (lastReminderHour == h) return;
  
  String msg;
  if (h == 7 && m >= 28 && m <= 32) {
    msg = "☀️ Guten Morgen!\nHeute:\n";
    if (taskCount == 0) {
      msg += "Keine Aufgaben – freier Tag!\n";
    } else {
      int showCount = min((int)taskCount, 10);
      for (int i = 0; i < showCount; i++) msg += "- " + taskList[i].name + "\n";
      if (taskCount > 10) msg += "…und " + String(taskCount - 10) + " weitere\n";
    }
    msg += "\nDenk an deine Habits 🌱";
  } else if ((h == 12 || h == 16) && m >= 28 && m <= 32) {
    msg = "🕒 Reminder (" + String(h) + ":30)\nTop 3:\n";
    for (int i = 0; i < min((int)taskCount, 3); i++) msg += String(i + 1) + ". " + taskList[i].name + "\n";
  } else if (h == 19 && m == 0) {
    msg = "🌙 Abend-Check\nErledigt heute: " + String(tasksDoneToday) + "\nNoch offen:\n";
    for (int i = 0; i < taskCount; i++) msg += "- " + taskList[i].name + "\n";
    int done = 0; for (int i = 0; i < MAX_HABITS; i++) if (habits[i].doneToday) done++;
    msg += "\n🌱 Habits: " + String(done) + "/" + String(MAX_HABITS) + " erledigt";
    tasksDoneToday = 0;
  }
  if (msg.length()) { sendTgMsg(CHAT_ID, msg); lastReminderHour = h; }
}

void checkMidnightReset() {
  struct tm ti; if (!getLocalTime(&ti)) return;
  if (ti.tm_hour == 0 && ti.tm_min == 0 && lastMidnightDay != ti.tm_yday) {
    lastMidnightDay = ti.tm_yday;
    String msg = "🌅 Neuer Tag!\n🌱 Habit-Streaks:\n";
    for (int i = 0; i < MAX_HABITS; i++) {
      if (habits[i].doneToday) {
        habits[i].streak++;
        msg += "✅ " + String(habits[i].name) + " – " + String(habits[i].streak) + "d 🔥\n";
      } else {
        if (habits[i].streak > 0) msg += "❌ " + String(habits[i].name) + " – Streak zurückgesetzt\n";
        habits[i].streak = 0;
      }
      habits[i].doneToday = false;
    }
    bcHabits(); sendTgMsg(CHAT_ID, msg);
    saveHabits();  // [DEMO-FEATURE] Persist streak changes to SPIFFS
  }
}

void checkStaleTasks() {
  if (millis() - lastStaleChk < 3600000UL) return; 
  lastStaleChk = millis();
  unsigned long now = millis();
  
  for (int i = 0; i < taskCount; i++) {
    if (!taskList[i].staleAlerted && (now - taskList[i].createdAt > 48UL * 3600UL * 1000UL) && taskList[i].score > 15.0f) {
      String p = "Mein Name ist YOUR_NAME. Die Aufgabe '" + taskList[i].name + "' liegt seit über 2 Tagen unerledigt (Score: " + String((int)taskList[i].score) + "). Schreibe eine kurze, motivierende Nachricht auf Deutsch und schlage vor, jetzt einen 25-Min Pomodoro zu starten.";
      
      // [FIX] Warnung nur als "versendet" markieren, wenn die KI den Auftrag annimmt!
      if (requestAI(AI_JOB_TELEGRAM, p, "llama-3.1-8b-instant")) {
         taskList[i].staleAlerted = true;
      }
    }
  }
}

void processTask(const String& cat, float weight, int days, const String& fachName) {
  isNewDataFlash = true;
  float katPts = (cat == "test") ? 50.f : (cat == "hausaufgabe" ? 20.f : 10.f);
  float score  = (katPts * weight) / (float)(days + 0.1f);

  String shortF = fachName.substring(0, 2); shortF.toUpperCase();
  String name   = shortF + "-" + cat.substring(0, 4); name.toUpperCase();
  
  if (taskCount < 75) {
    taskList[taskCount] = {name, score, millis(), false}; taskCount++;
  } else {
    if (score <= taskList[taskCount - 1].score) { updateDisplay(); return; }
    taskList[taskCount - 1] = {name, score, millis(), false};
  }

  Task key = taskList[taskCount - 1]; int j = taskCount - 2;
  while (j >= 0 && taskList[j].score < key.score) { taskList[j + 1] = taskList[j]; j--; }
  taskList[j + 1] = key;

  updateDisplay();
  tasksChanged = true; 
}

// ============================================================
//  HTTP HANDLER
// ============================================================
void handleRoot()     { server.send_P(200, "text/html",        htmlPage); }
void handleManifest() { server.send_P(200, "application/json", manifestJson);}
void handleSW()       { server.send_P(200, "application/javascript", swJs); }
void handleStatus()   { server.send(200, "application/json", "{\"checkedIn\":" + String(checkedIn) + ",\"ram\":" + String(ESP.getFreeHeap()) + "}"); }
void handleSetCheckIn() { checkedIn = 1; server.send(200, "text/plain", "OK"); wakeUpDisplay(); bcLock(1); }

// [V29-BLOCK0] SPIFFS Test Handler
void handleSPIFFSTest() {
  if (!SPIFFS.begin(true)) {
    server.send(500, "text/plain", "SPIFFS Mount fehlgeschlagen");
    return;
  }

  // Write test
  File testFile = SPIFFS.open("/test_v29.txt", "w");
  if (!testFile) {
    server.send(500, "text/plain", "SPIFFS Write fehlgeschlagen");
    return;
  }
  testFile.print("SPIFFS OK @ ");
  testFile.println(millis());
  testFile.close();

  // Read test
  testFile = SPIFFS.open("/test_v29.txt", "r");
  if (!testFile) {
    server.send(500, "text/plain", "SPIFFS Read fehlgeschlagen");
    return;
  }
  String content = testFile.readString();
  testFile.close();

  // List test (ESP32 SPIFFS API)
  File root = SPIFFS.open("/");
  int fileCount = 0;
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      fileCount++;
      f = root.openNextFile();
    }
  }

  String response = "✅ SPIFFS OK\n";
  response += "Files: " + String(fileCount) + "\n";
  response += "Test content: " + content;
  server.send(200, "text/plain", response);
  Serial.println("[SPIFFS] Test erfolgreich");
}

// ============================================================
//  TELEGRAM NACHRICHTENHANDLER
// ============================================================
void handleNewMessages(int num) {
  for (int i = 0; i < num; i++) {
    String chat_id = bot.messages[i].chat_id;
    if (chat_id != CHAT_ID) continue;
    String text = bot.messages[i].text;
    bool   isCB = (bot.messages[i].type == "callback_query");
    
    wakeUpDisplay();
    
    if (text == "/start") {
      sendTgMenu(chat_id, "Willkommen beim Assistant Hub Pro!\nWähle eine Aktion aus dem Menü unten.");
    }
    // [DEMO-FEATURE] SECRET COMMANDS FOR PRESENTATION
    else if (text == "/giverandtask") {
      // Add 10 fake tasks
      String categories[] = {"todo", "hausaufgabe", "test"};
      String subjects[] = {"Mathe", "NT", "Englisch", "Andere"};
      int addedCount = 0;
      
      for (int j = 0; j < 10 && taskCount < 75; j++) {
        String cat = categories[random(3)];
        String subj = subjects[random(4)];
        String name = "Demo-Task #" + String(j+1) + " (" + subj + ")";
        
        taskList[taskCount] = {name, (float)(random(1,6)), millis(), false};
        taskList[taskCount].isFakeTask = true;  // ← Mark as fake
        taskCount++;
        addedCount++;
      }
      
      sendTgMsg(chat_id, "✅ " + String(addedCount) + " Demo-Tasks hinzugefügt!");
      bcChat("📋 " + String(addedCount) + " neue Demo-Tasks!");
      bcTasks();  // [DEMO-FEATURE] Update WebUI immediately
      Serial.printf("[DEMO] Added %d fake tasks\n", addedCount);
    }
    else if (text == "/deleterandtask") {
      // Delete all fake tasks (reverse iteration!)
      int deletedCount = 0;
      for (int j = taskCount - 1; j >= 0; j--) {
        if (taskList[j].isFakeTask) {
          // Shift array backwards
          for (int k = j; k < taskCount - 1; k++) {
            taskList[k] = taskList[k+1];
          }
          taskCount--;
          deletedCount++;
        }
      }
      
      sendTgMsg(chat_id, "✅ " + String(deletedCount) + " Demo-Tasks gelöscht!");
      bcChat("🗑️ Demo-Tasks entfernt!");
      bcTasks();  // [DEMO-FEATURE] Update WebUI immediately
      Serial.printf("[DEMO] Deleted %d fake tasks\n", deletedCount);
    }
    else if (text == "/editallstreaks") {
      // Start interactive streak editor
      telegramEditState = EDIT_SELECT_HABIT;
      selectedHabitIndex = -1;
      
      String msg = "Welchen Streak willst du editieren?\n\n";
      for (int j = 0; j < MAX_HABITS; j++) {
        msg += String(j+1) + "️⃣ <b>" + String(habits[j].name) + "</b> (" + String(habits[j].streak) + " Tage)\n";
      }
      msg += "\n→ Antworte mit 1-6";
      
      sendTgMsgHtml(chat_id, msg);
      Serial.printf("[DEMO] Started /editallstreaks interactive mode\n");
    }
    // [DEMO-FEATURE] Handle /editallstreaks state machine
    else if (telegramEditState == EDIT_SELECT_HABIT) {
      // Parse habit selection (1-6)
      int habitIdx = atoi(text.c_str()) - 1;
      if (habitIdx >= 0 && habitIdx < MAX_HABITS) {
        selectedHabitIndex = habitIdx;
        telegramEditState = EDIT_INPUT_VALUE;
        sendTgMsg(chat_id, "Wie viele Tage für <b>" + String(habits[habitIdx].name) + "</b>?");
        Serial.printf("[DEMO] User selected habit %d (%s)\n", habitIdx, habits[habitIdx].name);
      } else {
        sendTgMsg(chat_id, "❌ Ungültige Nummer. Bitte 1-6.");
        telegramEditState = EDIT_SELECT_HABIT;
      }
    }
    else if (telegramEditState == EDIT_INPUT_VALUE && selectedHabitIndex >= 0) {
      // Parse new streak value
      int newStreak = atoi(text.c_str());
      if (newStreak >= 0 && newStreak <= 365) {
        habits[selectedHabitIndex].streak = newStreak;
        saveHabits();  // ← Persist to SPIFFS
        
        String msg = "✅ <b>" + String(habits[selectedHabitIndex].name) + "</b> → " + String(newStreak) + " Tage";
        sendTgMsgHtml(chat_id, msg);
        bcStatus("Habit aktualisiert! " + String(habits[selectedHabitIndex].name) + ": " + String(newStreak) + " 🎯");
        
        Serial.printf("[DEMO] Updated habit %d: streak=%d\n", selectedHabitIndex, newStreak);
      } else {
        sendTgMsg(chat_id, "❌ Ungültige Eingabe. Bitte 0-365 Tage eingeben.");
      }
      
      telegramEditState = EDIT_IDLE;
      selectedHabitIndex = -1;
    }
    else if (text == "/setpomocount") {
      // Parse: /setpomocount 5
      String args = text.substring(16);  // Skip "/setpomocount "
      int newCount = atoi(args.c_str());
      pomodoroPhases = newCount;  // [DEMO-FEATURE] Set pomodoro count
      
      sendTgMsg(chat_id, "✅ Pomodoro-Counter → " + String(newCount));
      bcStatus("🍅 Pomodoro-Count: " + String(newCount));
      Serial.printf("[DEMO] Set pomodoro count to %d\n", newCount);
    }
    // [DEMO-FEATURE] Debug commands
    else if (text == "/heap") {
      String msg = "📊 <b>Heap Status</b>\n\n";
      msg += "Free: " + String(ESP.getFreeHeap()) + " B\n";
      msg += "MaxAlloc: " + String(ESP.getMaxAllocHeap()) + " B\n";
      msg += "MinFree: " + String(ESP.getMinFreeHeap()) + " B\n";
      msg += "Tasks: " + String(taskCount) + "/75\n";
      msg += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";
      
      sendTgMsgHtml(chat_id, msg);
    }
    else if (text == "/uptime") {
      unsigned long uptimeMin = millis() / 60000;
      unsigned long uptimeHours = uptimeMin / 60;
      unsigned long uptimeDays = uptimeHours / 24;
      
      String msg = "⏱️ <b>Uptime</b>\n" + String(uptimeDays) + "d " + String(uptimeHours % 24) + "h " + String(uptimeMin % 60) + "m";
      sendTgMsgHtml(chat_id, msg);
    }
    else if (text == "/restart") {
      sendTgMsg(chat_id, "🔄 Rebooting in 2 seconds...");
      // [RESTART-FIX] Set flag instead of delay+restart in handler
      // Prevents RTOS state corruption from blocking handler
      restartRequestTime = millis();
      restartRequested = true;
    }
    else if (text == "/checkout") {
      checkedIn = 0; bcLock(0);
      sendTgMenu(chat_id, "✅ Web-UI ist jetzt gesperrt.");
    }
    else if (text == "/testbot" || text == "📊 Status") {
      String msg = "🤖 <b>AssistantHub Status</b>\n\n<b>WLAN:</b> 🟢 " + WiFi.localIP().toString() + "\n<b>RAM:</b> " + String(ESP.getFreeHeap()) + " B\n<b>Max-Alloc:</b> " + String(ESP.getMaxAllocHeap()) + " B\n<b>Aufgaben:</b> " + String(taskCount) + "/75\n<b>Web-UI:</b> " + String(checkedIn ? "✅ Aktiv" : "🔒 Gesperrt");
      sendTgMsgHtml(chat_id, msg);
    }
    else if (text == "/aufgaben" || text == "-Aufgaben-") {
      String msg = "📝 Deine Aufgaben:\n";
      if (taskCount == 0) msg = "Alles erledigt! 🎉";
      else for(int j=0; j<taskCount; j++) msg += String(j+1) + ". " + taskList[j].name + " (Score: " + String((int)taskList[j].score) + ")\n";
      sendTgMsg(chat_id, msg);
    }
    else if (text == "⏱ Pomodoro") {
       sendTgMsg(chat_id, "Nutze das Web-Interface, um den Pomodoro präzise zu steuern.");
    }
    else if (text == "KI-Frage") {
       sendTgMsg(chat_id, "Schreibe einfach '/ask Deine Frage' oder '/deep Dein Thema' hier in den Chat!");
    }
    else if (text == "/finish" || text == "Erledigt") {
      if (taskCount == 0) { sendTgMsg(chat_id, "Nichts zu tun."); continue; }
      {
        DynamicJsonDocument kbDoc(4096);
        JsonArray kbRows = kbDoc.to<JsonArray>();
        for (int j = 0; j < taskCount; j++) {
          JsonArray row = kbRows.createNestedArray();
          JsonObject btn = row.createNestedObject();
          btn["text"] = taskList[j].name;
          btn["callback_data"] = "del_" + String(j);
        }
        String kb; serializeJson(kbRows, kb);
        bot.sendMessageWithInlineKeyboard(chat_id, "Was hast du erledigt?", "", kb);
      }
    }
    else if (isCB && text.startsWith("del_")) {
      int idx = text.substring(4).toInt();
      if (idx >= 0 && idx < taskCount) {
        for (int j = idx; j < taskCount - 1; j++) taskList[j] = taskList[j+1];
        taskCount--; tasksDoneToday++; updateDisplay(); tasksChanged = true;
        sendTgMsg(chat_id, "✅ Erledigt und synchronisiert!");
      }
    }
    else if (text == "/new" || text == "➕ Neu") {
      bot.sendMessageWithInlineKeyboard(chat_id, "Typ:", "", "[[{\"text\":\"To-Do\",\"callback_data\":\"cat_todo\"}],[{\"text\":\"Hausaufgabe\",\"callback_data\":\"cat_hausaufgabe\"}],[{\"text\":\"Test\",\"callback_data\":\"cat_test\"}]]");
    }
    else if (isCB && text.startsWith("cat_")) {
      tempCat = text.substring(4);
      tempCatTime = millis();
      if (tempCat == "todo") bot.sendMessageWithInlineKeyboard(chat_id, "Priorität:", "", "[[{\"text\":\"Hoch\",\"callback_data\":\"fach_1.5_Wichtig\"}],[{\"text\":\"Normal\",\"callback_data\":\"fach_1.0_Normal\"}],[{\"text\":\"Tief\",\"callback_data\":\"fach_0.5_Unwichtig\"}]]");
      else bot.sendMessageWithInlineKeyboard(chat_id, "Fach:", "", "[[{\"text\":\"Mathe\",\"callback_data\":\"fach_1.5_Mathe\"},{\"text\":\"NT\",\"callback_data\":\"fach_1.4_NT\"}],[{\"text\":\"Englisch\",\"callback_data\":\"fach_1.3_Engl\"},{\"text\":\"Andere\",\"callback_data\":\"fach_1.0_Andere\"}]]");
    }
    else if (isCB && text.startsWith("fach_")) {
      tempFach = text.substring(5);
      bot.sendMessageWithInlineKeyboard(chat_id, "Tage?", "", "[[{\"text\":\"1\",\"callback_data\":\"day_1\"},{\"text\":\"2\",\"callback_data\":\"day_2\"},{\"text\":\"3\",\"callback_data\":\"day_3\"}],[{\"text\":\"5\",\"callback_data\":\"day_5\"},{\"text\":\"7\",\"callback_data\":\"day_7\"}]]");
    }
    else if (isCB && text.startsWith("day_")) {
      int sp = tempFach.indexOf('_');
      if (sp < 0 || tempCat.length() == 0 || millis() - tempCatTime > 300000UL) { sendTgMsg(chat_id, "⚠️ Session abgelaufen. Bitte ➕ Neu neu starten."); continue; }
      int days = text.substring(4).toInt();
      processTask(tempCat, tempFach.substring(0, sp).toFloat(), max(days, 1), tempFach.substring(sp+1));
      sendTgMsg(chat_id, "✅ Aufgabe synchronisiert!");
    }
    else if (text.startsWith("/ask ") || text.startsWith("/ask\n")) {
      if (checkedIn == 1) {
        sendTgMsg(chat_id, "ℹ️ Du bist im Web eingecheckt. Die KI-Anfrage wird trotzdem verarbeitet.\nZum Auschecken: /checkout");
      }
      String frage = text.substring(5); frage.trim();
      if (frage.length() == 0) continue;

      // [FINALFIXES-BUG3-DIAG] Send initial thinking message and capture its ID
      int mid = sendTgMsg(chat_id, "🤔 Überlege…");
      Serial.printf("[TELEGRAM-ASK] Initial message ID from sendTgMsg: %d\n", mid);

      if (mid < 1) {
        Serial.printf("[TELEGRAM-ASK] CRITICAL: sendTgMsg failed (mid=%d), cannot schedule request\n", mid);
        continue;  // sendTgMsg failed, bail out
      }

      String prompt = "Mein Name ist YOUR_NAME. Antworte in max. 2 Sätzen, freundlich, auf Deutsch. Frage: " + frage;

      Serial.printf("[TELEGRAM-ASK] Scheduling AI request with mid=%d, prompt=%.50s...\n", mid, prompt.c_str());

      if (!requestAI(AI_JOB_TELEGRAM, prompt, "llama-3.1-8b-instant", mid)) {
         Serial.printf("[TELEGRAM-ASK] requestAI returned false, checking heap\n");

         // [CRITICAL] Provide user feedback about the failure
         if (mid > 0) {
           String errMsg = "⚠️ KI ist ausgelastet (Heap: ";
           errMsg += String(ESP.getFreeHeap());
           errMsg += " B). Bitte versuche es gleich nochmal.";
           editTgMsg(chat_id, mid, errMsg);
         }
      } else {
        Serial.printf("[TELEGRAM-ASK] requestAI accepted, waiting for response...\n");
      }
    }
    else if (text.startsWith("/deep ") || text.startsWith("/deep\n")) {
      if (checkedIn == 1) {
        sendTgMsg(chat_id, "ℹ️ Du bist im Web eingecheckt. Die Tiefrecherche wird trotzdem verarbeitet.\nZum Auschecken: /checkout");
      }
      String frage = text.substring(6); frage.trim();
      if (frage.length() == 0) continue;

      // Fix 3 + 4: Kürze Frage aggressiv wenn > 500 Zeichen (HTTP 413 Prevention)
      if (frage.length() > 500) {
        Serial.printf("[DEEP] Frage zu lang (%d > 500), kürze.\n", frage.length());
        frage = frage.substring(0, 497) + "...";
      }

      // [FINALFIXES-BUG3-DIAG] Send initial thinking message and capture its ID
      int mid = sendTgMsg(chat_id, "🌍 Tiefrecherche läuft…");
      Serial.printf("[TELEGRAM-DEEP] Initial message ID from sendTgMsg: %d\n", mid);

      if (mid < 1) {
        Serial.printf("[TELEGRAM-DEEP] CRITICAL: sendTgMsg failed (mid=%d), cannot schedule request\n", mid);
        continue;  // sendTgMsg failed, bail out
      }

      // [V29] Telegram Deep-Prompt komprimiert
      String prompt = "Recherche, strukturiert (## Ueberschriften), Deutsch, max 450 W, ## Fazit. Thema: " + frage;
      Serial.printf("[TELEGRAM-DEEP] Scheduling request with mid=%d, prompt-length=%d\n", mid, prompt.length());

      // [V25.5] compound-beta ist der korrekte Groq-API-Modellname
      if (!requestAI(AI_JOB_TELEGRAM, prompt, "llama-3.3-70b-versatile", mid)) {
         Serial.printf("[TELEGRAM-DEEP] requestAI returned false\n");

         // [CRITICAL] Provide user feedback about the failure
         if (mid > 0) {
           String errMsg = "⚠️ KI ist ausgelastet (Heap: ";
           errMsg += String(ESP.getFreeHeap());
           errMsg += " B). Bitte versuche es gleich nochmal.";
           editTgMsg(chat_id, mid, errMsg);
         }
      } else {
        Serial.printf("[TELEGRAM-DEEP] requestAI accepted, waiting for response...\n");
      }
    }
  }
}

// ============================================================
//  NETWORK TASK (Core 0)
// ============================================================
void networkTaskLogic(void*) {
  vTaskDelay(pdMS_TO_TICKS(50));  // Let task fully initialize

  // [ENGINEER-FIX1] Boot-Message Retry-Fallback mit atomic check
  // Race condition gelöst: setup() setzt bootNotifyPending=false NACH erfolgreichem Send.
  // Wenn setup() failed, bleibt es true und networkTask retries.
  // taskENTER_CRITICAL guarantee atomic read-modify-write on dual-core ESP32-S3.
  bool shouldRetry = false;
  taskENTER_CRITICAL(&bootNotifyMutex);
  if (bootNotifyPending) {
    bootNotifyPending = false;  // Atomic: claim the retry token
    shouldRetry = true;
  }
  taskEXIT_CRITICAL(&bootNotifyMutex);
  
  if (shouldRetry) {
    vTaskDelay(pdMS_TO_TICKS(3000));  // Wait for heap to stabilize
    Serial.printf("[BOOT-RETRY] Heap: %u B, MaxAlloc: %u B\n", 
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    int msgId = sendTgMsgHtml(CHAT_ID, "🚀 <b>Online (delayed)</b>\nIP: <code>" + WiFi.localIP().toString() + "</code>");
    if (msgId > 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      sendTgMenu(CHAT_ID, "Menü:");
      Serial.println("[BOOT-RETRY] ✅ Sent successfully");
    } else {
      Serial.printf("[BOOT-RETRY] ⚠️ Failed (msgId=%d) – continuing without boot message\n", msgId);
    }
  }

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect...");
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(5000));
      if (WiFi.status() == WL_CONNECTED) { WiFi.setSleep(false); esp_wifi_set_ps(WIFI_PS_NONE); }
      continue;
    }

    server.handleClient();

    // [TIER2] WebSocket only if not in critical TLS mode
    if (webSocketTaskEnabled) {
      webSocket.loop();
    }

    if (millis() - lastBotClientReset > 50000) { client.stop(); lastBotClientReset = millis(); }

    // [TIER2] Telegram polling only if not in critical TLS mode
    // [FINAL-FIX] Threshold gesenkt 35KB → 10KB (mit BLE removed haben wir mehr Luft, aber wir bleiben tolerant)
    if (telegramTaskEnabled && millis() - lastTelegramCheck > 400) {
      uint32_t maxHeap = ESP.getMaxAllocHeap();
      if (maxHeap >= 10000) {
        int n = bot.getUpdates(bot.last_message_received + 1);
        if (n > 0) {
          Serial.printf("[TELEGRAM] Got %d update(s) [Heap: %u B]\n", n, maxHeap);
          while (n) { handleNewMessages(n); n = bot.getUpdates(bot.last_message_received + 1); }
        }
        checkTimeReminders();
        checkMidnightReset();
        checkStaleTasks();
      }
      lastTelegramCheck = millis();
    }

    // [TIER2] During critical mode, yield to allow aiTask to progress
    if (tlsCriticalMode) {
      vTaskDelay(pdMS_TO_TICKS(10));  // Brief sleep in critical mode
    }

    if (pomoChanged)  { bcPomo();  pomoChanged  = false; }
    if (tasksChanged) { bcTasks(); tasksChanged = false; }

    // [V28] Phase 3: rotierender Funny-Status, solange ein Job pendet.
    // Web: broadcast per WS (schnell, 2.5s). Telegram: editMessageText an der
    // EINEN Bubble (3s Budget, damit Telegram-Flood-Limit nicht zuschlägt).
    // Ein aiJob zur Zeit (pending-Flag), daher teilen sich beide den Timer.
    {
      xSemaphoreTake(aiJobMutex, portMAX_DELAY);
      bool pend  = aiJob.pending;
      int  jtype = aiJob.type;
      int  tid   = aiJob.telMsgId;
      xSemaphoreGive(aiJobMutex);

      uint32_t budget = (jtype == AI_JOB_TELEGRAM) ? 3000 : 2500;
      uint32_t minDelay = (jtype == AI_JOB_TELEGRAM) ? 1500 : 800;  // [FINAL-FIX] Initial delay
      if (pend && millis() - lastStatusBcTime >= budget &&
          millis() - lastStatusBcTime >= minDelay) {  // [FINAL-FIX] Double guard
        int idx;
        do { idx = esp_random() % FUNNY_STATUS_N; } while (idx == lastStatusIdx && FUNNY_STATUS_N > 1);
        lastStatusIdx = idx;
        if (jtype == AI_JOB_WEB) {
          bcStatus(String(FUNNY_STATUS[idx]));
        } else if (jtype == AI_JOB_TELEGRAM && tid > 0) {
          editTgMsg(CHAT_ID, tid, String("💭 ") + FUNNY_STATUS[idx] + "...");
        }
        lastStatusBcTime = millis();
      }
    }

    // [V21-FIX] *** KRITISCH: AI-Ergebnis abholen und zustellen ***
    // aiJob.done wurde in allen Vorgänger-Versionen nie ausgelesen → Ergebnis verschwand im Nichts.
    // Telegram blieb auf "Überlege…", WebUI auf "denkt nach…" – für immer.
    {
      xSemaphoreTake(aiJobMutex, portMAX_DELAY);
      bool rdy = aiJob.done;
      String aiTxt; int aiTp = 0; int aiTid = -1;
      if (rdy) {
        // [CRITICAL-FIX] Defensive NULL check to prevent strlen(NULL) crash
        if (aiJob.result) {
          aiTxt = String(aiJob.result);
        } else {
          aiTxt = "⚠️ KI-Antwort nicht verfügbar (interner Fehler).";
        }
        aiTp  = aiJob.type;
        aiTid = aiJob.telMsgId;
        aiJob.done = false; // Gelesen → zurücksetzen
        // [FIX2] aiJob.result is now static – no free() needed

        // [FINALFIXES-BUG3-DIAG] Log when response is ready
        Serial.printf("[AI-RESPONSE-READY] type=%d, telMsgId=%d, textLen=%d\n",
                      aiTp, aiTid, aiTxt.length());
      }
      xSemaphoreGive(aiJobMutex);
      if (rdy) {
        if (aiTp == AI_JOB_WEB) {
          Serial.println("[AI-RESPONSE] Delivering to Web UI");
          bcChat(aiTxt);
        }
        else if (aiTp == AI_JOB_TELEGRAM && aiTid > 0) {
          Serial.printf("[AI-RESPONSE] Delivering to Telegram (editing message %d)\n", aiTid);
          editTgMsg(CHAT_ID, aiTid, aiTxt);
        }
        else if (aiTp == AI_JOB_TELEGRAM && aiTid <= 0) {
          Serial.printf("[AI-RESPONSE] Telegram telMsgId=%d invalid, sending as new message\n", aiTid);
          sendTgMsg(CHAT_ID, aiTxt);
        }
      }
    }

    // Periodisches Heap-Trimming alle ~500ms (100 × 5ms) gegen Fragmentierung
    // malloc_trim(0) nicht auf Arduino/ESP verfügbar
    static uint8_t netLoopCnt = 0;
    if (++netLoopCnt >= 100) { /* malloc_trim(0); */ netLoopCnt = 0; }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  // [WDT-FIX] Task-WDT sofort deaktivieren – VOR allen blockierenden Operationen
  // (showStartupScreen 3s, WiFi-Connect bis 16s, setupPersistentTls bis 30s).
  // Ohne diesen Call kann TG1WDT im Verbindungsaufbau auslösen.
  esp_task_wdt_deinit();
  // pinMode(ledPin, OUTPUT);  // [LITE] LED deaktiviert

  // [V29-BLOCK1] LED-Controller setup: Green atmet kontinuierlich, Red for Pomodoro
  // [DISABLED] causing TG1WDT crash during init
  // ledCtrl.addBreather(LED_GREEN, 2000);  // Idle: Green atmet (2s Periode)
  // ledCtrl.addBreather(LED_RED, 2000);    // Pomodoro: Red synchronized (auch 2s, aber wird nur aktiv bei Pomodoro)
  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);

  displayMutex = xSemaphoreCreateMutex();
  aiJobMutex   = xSemaphoreCreateMutex();

  // [STARTUP-UI] Show welcome screen
  showStartupScreen();

  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(SSIDhot, Passworthot);
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  }

  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);

  // [CRITICAL-FIX] Configure explicit DNS servers for reliable api.groq.com resolution
  // DHCP-provided DNS can be unreliable on marginal WiFi (-72 dBm).
  // Use Google Public DNS (8.8.8.8, 8.8.4.4) as fallback.
  IPAddress primaryDNS(8, 8, 8, 8);      // Google DNS Primary
  IPAddress secondaryDNS(8, 8, 4, 4);    // Google DNS Secondary
  if (!WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), primaryDNS, secondaryDNS)) {
    Serial.println("[WIFI] Failed to configure DNS servers");
  } else {
    Serial.println("[WIFI] Configured explicit DNS servers (8.8.8.8, 8.8.4.4)");
  }

  // Skip cert validation for Telegram bot (Jan 2025 root cert expired)
  client.setInsecure();
  client.setTimeout(5000);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  bot.longPoll = 0;

  // [DEMO-FEATURE] Load Habits from SPIFFS (Streak persistence after power loss)
  loadHabits();

  // [FINAL-FIX] BOOT-MESSAGE HIER (vor setupPersistentTls, vor Tasks)
  // Heap ist noch ~70-80 KB frisch, optimale Bedingungen für TLS-Handshake
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[BOOT] Sending boot notification (Heap: %u B, MaxAlloc: %u B)...\n", 
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    int msgId = sendTgMsgHtml(CHAT_ID, "🚀 <b>Assistant Hub Pro online!</b>\nIP: <code>" + WiFi.localIP().toString() + "</code>\n<i>BLE-free Edition</i>");
    if (msgId > 0) {
      Serial.printf("[BOOT] ✅ Sent successfully! msgId=%d\n", msgId);
      delay(200);
      sendTgMenu(CHAT_ID, "Menü:");
    } else {
      Serial.printf("[BOOT] ⚠️ Failed (msgId=%d), Heap: %u B – will retry from networkTask\n", 
                    msgId, ESP.getFreeHeap());
    }
  } else {
    Serial.println("[BOOT] WiFi not connected, skipping boot notification");
  }

  // [SOCKET-RESET] Initialize persistent TLS for AI requests
  setupPersistentTls();

  // [BLE-REMOVED] BLE init block removed - frees ~30-40 KB heap

  server.on("/", HTTP_GET, handleRoot);
  server.on("/manifest.json", HTTP_GET, handleManifest);
  server.on("/sw.js", HTTP_GET, handleSW);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/setCheckIn", HTTP_GET, handleSetCheckIn);
  server.on("/test-spiffs", HTTP_GET, handleSPIFFSTest);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // [FIX-WEBUI] Tasks ZUERST erstellen → server.handleClient() läuft ab sofort auf Core 0.
  xTaskCreatePinnedToCore(networkTaskLogic, "Net", 20480, NULL, 2, &NetworkTask, 0);
  xTaskCreatePinnedToCore(aiTask, "AI", 65536, NULL, 1, &AITask, 1);
  bootNotifyPending = false;  // [FINAL-FIX] Boot-Message bereits oben gesendet

  // [REFACTOR-V31] TLS-Warmup entfernt – bringt kein Performance-Gain und riskiert TG1WDT
  // Streaming mit SSE auf jeden Call ist effizienter

  lastInteractionTime = millis();
}

// ============================================================
//  LOOP (Core 1)
// ============================================================
void loop() {
  unsigned long now = millis();

  // [V29-BLOCK1] LED-Controller update (breathing + blinking)
  // ledCtrl.update();  // [LITE] LED deaktiviert – einkommentieren für LED-Support

  // [FINAL-FIX] Heap telemetry every 30s - for monitoring memory health
  static unsigned long lastHeapLog = 0;
  if (now - lastHeapLog > 30000) {
    lastHeapLog = now;
    Serial.printf("[HEAP] Free=%u B, MaxAlloc=%u B, MinFree=%u B, Uptime=%lu min\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(), now / 60000);
  }

  if (now - lastInteractionTime > 10000) {
    if (!screensaverActive) {
      screensaverActive = true;
      if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        display.clearDisplay(); display.display();
        xSemaphoreGive(displayMutex);
      }
    }
    if (now - lastSSUpdate > 60) {
       if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
         display.clearDisplay();
         display.drawCircle(64, 32, ssRadius, SSD1306_WHITE);
         ssRadius += ssDir;
         if (ssRadius > 28 || ssRadius < 5) ssDir = -ssDir;
         display.display();
         xSemaphoreGive(displayMutex);
       }
       lastSSUpdate = now;
    }
  }

  if (pomodoroActive && now > pomodoroEndTime) {
    if (pomodoroBreak) {
       pomodoroBreak = false;
       pomodoroEndTime = now + POMO_DURATION;
       sendTgMsgHtml(CHAT_ID, "🚀 <b>Pause beendet!</b> Nächster Fokus-Sprint startet.");
       pomoChanged = true;
    } else {
       pomodoroPhases--;
       if (pomodoroPhases > 0) {
         pomodoroBreak = true;
         pomodoroEndTime = now + POMO_BREAK_DUR;
         sendTgMsgHtml(CHAT_ID, "☕ <b>Phase geschafft!</b> 5 Min Pause. ⏸");
       } else {
         pomodoroActive = false;
         sendTgMsgHtml(CHAT_ID, "🏆 <b>Pomodoro-Session abgeschlossen!</b> Gut gemacht!");
       }
       pomoChanged = true;
       wakeUpDisplay();
    }
  }

  if (!screensaverActive && pomodoroActive && now - lastDisplayTick > 1000) {
     updateDisplay();
     lastDisplayTick = now;
  }

  if (pomodoroActive && now - lastPomoWsBc > 1000) {
      pomoChanged = true;
      lastPomoWsBc = now;
  }

  // [BLE-REMOVED] BLE connection state lines removed from loop

  if (isNewDataFlash && flashRemain == 0) { flashRemain = 10; flashTimer = now; isNewDataFlash = false; }
  if (flashRemain > 0 && now - flashTimer >= 40) {
    digitalWrite(ledPin, (flashRemain % 2 == 0) ? LOW : HIGH);
    flashRemain--;
    flashTimer = now;
    if (flashRemain == 0) digitalWrite(ledPin, LOW);
  }
  // Schnelleres Pulsieren während KI-Anfrage läuft (visuelles Feedback)
  unsigned long breathInterval = aiJob.pending ? 12 : 30;
  if (flashRemain == 0 && now - lastUpdate > breathInterval) {
    analogWrite(ledPin, brightness);
    brightness += fadeAmount;
    if (brightness <= 0 || brightness >= 255) fadeAmount = -fadeAmount;
    lastUpdate = now;
  }

  // [RESTART-FIX] Safe restart from main loop after 2s delay
  // Prevents RTOS state corruption by executing from loop() instead of handler
  if (restartRequested && (now - restartRequestTime >= 2000)) {
    Serial.println("[RESTART-FIX] Executing safe restart...");
    esp_task_wdt_deinit();  // Disable watchdog to avoid TG1WDT
    vTaskDelay(pdMS_TO_TICKS(500));  // Let tasks settle
    ESP.restart();  // Safe restart point
  }

  delay(16);
}