#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
// FluxGarage_RoboEyes defines bare macros N/E/S/W — must come after all other headers
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include <time.h>
#include <driver/i2s.h>

// const char* ssid     = "Veedu";
// const char* password = "Password@123";

const char* ssid     = "ACV-Auctions";
const char* password = "FerrariStradale@26";

// Firebase Realtime Database URL — replace with your project ID
#define FIREBASE_DB_URL "https://desk-buddy-007-default-rtdb.asia-southeast1.firebasedatabase.app"
#define GITHUB_POLL_INTERVAL_MS (15UL * 1000)

WebServer server(80);
static SemaphoreHandle_t stateMux = NULL;

#define TOUCH_PIN 4


// -------- MAX98357A SPEAKER PINS --------
#define SPK_BCLK  26   // I2S bit clock
#define SPK_LRC   27   // I2S left/right clock
#define SPK_DIN   33   // I2S data out → MAX98357A DIN
#define SPK_PORT  I2S_NUM_1
#define SPK_SAMPLE_RATE 44100
#define SPK_VOLUME      0.40f  // 40% — comfortable volume for 0.5W / 8Ω speaker

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// -------- MODES --------
enum Mode {
  MODE_AUTO,
  MODE_STATUS,
  MODE_POMODORO_READY,     // on page, timer not started
  MODE_POMODORO_FOCUS,
  MODE_POMODORO_BREAK,
  MODE_POMODORO_OVERTIME,
  MODE_CLOCK,
  MODE_PET,
  MODE_SLEEP_TRANSITION,   // eyes slowly close before sleep
  MODE_SLEEP,
  MODE_NOTIFICATION        // GitHub event overlay (auto-dismisses after 5s)
};
Mode currentMode = MODE_AUTO;

// -------- POMODORO --------
#define POMODORO_FOCUS_MS  (25UL * 1000)
#define POMODORO_BREAK_MS  (5UL  * 1000)
unsigned long pomodoroStartTime = 0;

// -------- AUTO MODE --------
int  lastAutoMood     = -1;
bool autoSleepActive  = false;

// -------- MOOD + AMBIENT TRACKING --------
int  trackedMood    = TIRED;
bool ambientActive  = false;

void setMoodTracked(int mood) {
  trackedMood = mood;
  roboEyes.setMood(mood);
}

void applyAmbient() {
  ambientActive = true;
  roboEyes.setCuriosity(true);
  roboEyes.setIdleMode(ON, 2, 3);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setSweat(false);
}

void clearAmbient() {
  ambientActive = false;
  roboEyes.setCuriosity(false);
  roboEyes.setIdleMode(OFF);
  roboEyes.setSweat(false);
}

// -------- INACTIVITY SLEEP --------
#define INACTIVITY_SLEEP_MS   (10UL * 60 * 1000)  // 10 minutes
#define SLEEP_TRANSITION_MS   1500
unsigned long lastActivityMs    = 0;
unsigned long sleepTransStart   = 0;

void enterSleep() {
  currentMode     = MODE_SLEEP_TRANSITION;
  sleepTransStart = millis();
  clearAmbient();
  setMoodTracked(TIRED);
}

// -------- PHYSICAL TOUCH TAP --------
int           touchTapCount  = 0;
bool          touchLastState = false;
unsigned long touchLastTap   = 0;
#define TOUCH_WINDOW_MS 400

// Single-tap cycles through: Clock → Pomodoro (ready, not started) → Eyes (auto)
const Mode TAP_CYCLE[] = { MODE_CLOCK, MODE_POMODORO_READY, MODE_AUTO };
const int  TAP_CYCLE_LEN = 3;
int        tapCycleIndex = -1;

void cycleTapMode() {
  tapCycleIndex = (tapCycleIndex + 1) % TAP_CYCLE_LEN;
  Mode next = TAP_CYCLE[tapCycleIndex];

  if (next == MODE_CLOCK) {
    currentMode = MODE_CLOCK;
    clearAmbient();
  } else if (next == MODE_POMODORO_READY) {
    currentMode = MODE_POMODORO_READY;
    clearAmbient();
    setMoodTracked(ANGRY);
    roboEyes.setPosition(S);
    roboEyes.setAutoblinker(ON, 8, 0);
  } else if (next == MODE_AUTO) {
    currentMode     = MODE_AUTO;
    lastAutoMood    = -1;
    autoSleepActive = false;
    clearAmbient();
    applyAmbient();
  }

  Serial.printf("[TOUCH] Cycle → mode %d (index %d)\n", next, tapCycleIndex);
}

void handleTouchSensor() {
  bool touchNow = digitalRead(TOUCH_PIN);
  if (touchNow && !touchLastState) {
    touchTapCount++;
    touchLastTap = millis();
    lastActivityMs = millis();
    Serial.printf("[TOUCH] Rising edge detected — tap #%d\n", touchTapCount);
  }
  if (!touchNow && touchLastState) {
    Serial.println("[TOUCH] Released");
  }
  touchLastState = touchNow;
  if (touchTapCount > 0 && millis() - touchLastTap >= TOUCH_WINDOW_MS) {
    Serial.printf("[TOUCH] Window closed — firing enterPetMode(%d)\n", touchTapCount);
    enterPetMode(touchTapCount);
    touchTapCount = 0;
  }
}

// -------- PET / TOUCH SIM --------
enum PetAction { PET_NONE, PET_HAPPY, PET_LAUGH, PET_CONFUSED };
PetAction     petAction     = PET_NONE;
Mode          prePetMode    = MODE_AUTO;
int           prePetMood    = TIRED;
bool          prePetAmbient = false;
int           petPhase      = 0;
unsigned long petTimer1     = 0;
unsigned long petTimer2     = 0;

// -------- GITHUB NOTIFICATION --------
#define NOTIF_PHASE0_MS  2000   // eyes animate with reaction
#define NOTIF_PHASE1_MS  3500   // full-screen text
#define NOTIF_PHASE2_MS  2500   // eyes animate again, then restore

char          notifLabel[22]        = "";  // event type label
char          notifMsg[51]          = "";  // full message (up to 50 chars)
char          notifAuthor[22]       = "";  // author name
int           notifPhase            = 0;   // 0=eyes, 1=fullscreen, 2=eyes
unsigned long notifPhaseTimer       = 0;
bool          notifPositive         = true; // true=happy/VFlicker, false=tired/HFlicker
unsigned long lastSeenTimestamp     = 0;
volatile int8_t audioRequest        = 0;  // 1=positive chime, -1=negative chime
Mode          preNotifMode          = MODE_AUTO;
int           preNotifMood          = TIRED;
bool          preNotifAmbient       = false;

void restoreFromPet() {
  petAction   = PET_NONE;
  petPhase    = 0;
  currentMode = prePetMode;
  roboEyes.setVFlicker(false);
  roboEyes.setHFlicker(false);
  roboEyes.setSweat(false);
  roboEyes.setPosition(DEFAULT);
  setMoodTracked(prePetMood);
  if (prePetAmbient) applyAmbient();
  if (prePetMode == MODE_AUTO) lastAutoMood = -1;
}

bool inPomodoroMode() {
  return currentMode == MODE_POMODORO_READY  ||
         currentMode == MODE_POMODORO_FOCUS  ||
         currentMode == MODE_POMODORO_BREAK  ||
         currentMode == MODE_POMODORO_OVERTIME;
}

bool inEyesMode() {
  return currentMode == MODE_AUTO || currentMode == MODE_STATUS;
}

void enterPetMode(int taps) {
  // Dismiss notification on any tap
  if (currentMode == MODE_NOTIFICATION) {
    roboEyes.setVFlicker(false);
    roboEyes.setHFlicker(false);
    currentMode = preNotifMode;
    setMoodTracked(preNotifMood);
    if (preNotifAmbient) applyAmbient();
    if (preNotifMode == MODE_AUTO) lastAutoMood = -1;
    return;
  }

  // Any tap while sleeping (or transitioning) wakes to In Meeting mode
  if (currentMode == MODE_SLEEP || currentMode == MODE_SLEEP_TRANSITION) {
    currentMode     = MODE_STATUS;
    autoSleepActive = false;
    clearAmbient();
    setMoodTracked(DEFAULT);
    roboEyes.setCuriosity(true);
    roboEyes.setIdleMode(ON, 1, 1);
    roboEyes.setAutoblinker(ON, 4, 2);
    lastActivityMs = millis();
    return;
  }

  if (currentMode == MODE_PET) return; // ignore taps during active reaction

  if (taps == 1) { cycleTapMode(); return; }

  if (taps == 2) {
    if (inPomodoroMode()) {
      // Start Pomodoro from ready/any pomodoro state
      currentMode       = MODE_POMODORO_FOCUS;
      pomodoroStartTime = millis();
      clearAmbient();
      setMoodTracked(ANGRY);
      roboEyes.setPosition(S);
      roboEyes.setAutoblinker(ON, 8, 0);
      Serial.println("[TOUCH] Pomodoro started");
    } else if (inEyesMode()) {
      // Laugh reaction
      prePetMode = currentMode; prePetMood = trackedMood; prePetAmbient = ambientActive;
      currentMode = MODE_PET; clearAmbient();
      petAction = PET_LAUGH;
      roboEyes.setPosition(DEFAULT);
      setMoodTracked(HAPPY);
      roboEyes.setVFlicker(true, 5);
      petTimer1 = millis() + 3000;
      audioRequest = 2; // robot smile sound
    }
    return;
  }

  // 3+ taps
  if (inPomodoroMode()) {
    // Reset Pomodoro back to ready state
    currentMode = MODE_POMODORO_READY;
    clearAmbient();
    setMoodTracked(ANGRY);
    roboEyes.setPosition(S);
    roboEyes.setAutoblinker(ON, 8, 0);
    Serial.println("[TOUCH] Pomodoro reset to ready");
  } else if (inEyesMode()) {
    // Confused reaction
    prePetMode = currentMode; prePetMood = trackedMood; prePetAmbient = ambientActive;
    currentMode = MODE_PET; clearAmbient();
    petAction = PET_CONFUSED;
    petPhase  = 0;
    roboEyes.setPosition(DEFAULT);
    roboEyes.setHFlicker(true, 20);
    petTimer1 = millis() + 2000;
    audioRequest = 3; // angry wobble → harsh tone
  }
}


// -------- TIME HELPERS --------

int getTimeBasedMood() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return DEFAULT;
  int h = timeinfo.tm_hour;
  if (h >= 7  && h < 9)  return TIRED;
  if (h >= 9  && h < 12) return DEFAULT;
  if (h >= 12 && h < 13) return DEFAULT;
  if (h >= 13 && h < 18) return DEFAULT;
  if (h >= 18 && h < 21) return HAPPY;
  return DEFAULT; // fallback (sleep hours handled separately)
}

void showTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.printf("%02d-%02d-%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
  display.display();
}

// -------- SLEEP FACE --------
// Eye geometry mirrors the roboEyes defaults + Y_SHIFT of 5
#define SLEEP_EYE_W   36
#define SLEEP_EYE_H    4
#define SLEEP_EYE_LX  23
#define SLEEP_EYE_RX  69
#define SLEEP_EYE_Y   35  // vertical center of where eyes sit

void drawSleepFace() {
  display.clearDisplay();

  // Dashed eyes — thin horizontal bars
  display.fillRoundRect(SLEEP_EYE_LX, SLEEP_EYE_Y, SLEEP_EYE_W, SLEEP_EYE_H, 2, SSD1306_WHITE);
  display.fillRoundRect(SLEEP_EYE_RX, SLEEP_EYE_Y, SLEEP_EYE_W, SLEEP_EYE_H, 2, SSD1306_WHITE);

  // Floating Zzz — three characters drifting upward slowly
  int drift = (millis() / 600) % 10;  // cycles 0-9, moves up one pixel every 600ms

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);                          // mid z
  display.setCursor(97, 22 - drift);
  display.print("z");

  display.setTextSize(1);                          // small z — top
  display.setCursor(106, 15 - drift);
  display.print("z");

  display.display();
}

// -------- POMODORO OVERLAY --------
// Draws a small timer strip at the very top of the screen (y=0..9).
// Called after roboEyes draws to the buffer, before we flush.
void drawPomOverlay() {
  if (currentMode != MODE_POMODORO_READY   &&
      currentMode != MODE_POMODORO_FOCUS   &&
      currentMode != MODE_POMODORO_BREAK   &&
      currentMode != MODE_POMODORO_OVERTIME) return;

  if (currentMode == MODE_POMODORO_READY) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(31, 1);
    display.printf("FOCUS %02d:%02d", (int)(POMODORO_FOCUS_MS / 60000), (int)((POMODORO_FOCUS_MS % 60000) / 1000));
    return;
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (currentMode == MODE_POMODORO_OVERTIME) {
    display.setCursor(22, 1);
    display.print("Back to work!");
    return;
  }

  unsigned long totalMs  = (currentMode == MODE_POMODORO_FOCUS) ? POMODORO_FOCUS_MS : POMODORO_BREAK_MS;
  unsigned long elapsed  = millis() - pomodoroStartTime;
  unsigned long remainMs = (elapsed < totalMs) ? (totalMs - elapsed) : 0;
  unsigned int  mins     = remainMs / 60000;
  unsigned int  secs     = (remainMs % 60000) / 1000;

  // Label + time: "FOCUS 25:00" or "BREAK 05:00" — 11 chars * 6px = 66px, centered
  const char* label = (currentMode == MODE_POMODORO_FOCUS) ? "FOCUS" : "BREAK";
  display.setCursor(31, 1);
  display.printf("%s %02d:%02d", label, mins, secs);

  // Progress bar at y=9, 1px tall
  int barW = (int)(128.0f * (float)elapsed / (float)totalMs);
  if (barW > 128) barW = 128;
  display.fillRect(0, 9, barW, 1, SSD1306_WHITE);
}

// -------- GITHUB NOTIFICATION FULL SCREEN --------

// Word-wrap: splits text into two lines at a space boundary, max maxLen chars per line.
void wordWrap(const char* text, char* line1, char* line2, int maxLen) {
  int len = strlen(text);
  if (len <= maxLen) {
    strncpy(line1, text, maxLen); line1[maxLen] = '\0';
    line2[0] = '\0';
    return;
  }
  // Find last space at or before maxLen to break cleanly
  int split = maxLen;
  for (int i = maxLen - 1; i > 0; i--) {
    if (text[i] == ' ') { split = i; break; }
  }
  strncpy(line1, text, split); line1[split] = '\0';
  const char* rest = text + split;
  while (*rest == ' ') rest++; // skip leading space on line 2
  strncpy(line2, rest, maxLen); line2[maxLen] = '\0';
}

// Phase 1: clears the entire display and shows event info as large text.
void drawNotifFullScreen() {
  display.clearDisplay();

  // Inverted header bar
  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print(notifLabel);

  // Message — word-wrapped to two lines of 21 chars
  display.setTextColor(SSD1306_WHITE);
  char msgLine1[22], msgLine2[22];
  wordWrap(notifMsg, msgLine1, msgLine2, 21);

  display.setCursor(2, 16);
  display.print(msgLine1);
  if (msgLine2[0] != '\0') {
    display.setCursor(2, 26);
    display.print(msgLine2);
  }

  // Author
  display.setCursor(2, 48);
  display.print("by: ");
  display.print(notifAuthor);

  display.display();
}

// -------- GITHUB NOTIFICATION TRIGGER --------
// Called from poll task while holding stateMux.
void triggerNotification(const char* type, const char* msg, const char* author) {
  preNotifMode    = currentMode;
  preNotifMood    = trackedMood;
  preNotifAmbient = ambientActive;

  currentMode   = MODE_NOTIFICATION;
  notifPhase    = 0;
  notifPhaseTimer = millis() + NOTIF_PHASE0_MS;

  // Positive events: happy + vertical flicker. Negative (pr_comment): tired + horizontal.
  notifPositive = (strcmp(type, "pr_comment") != 0);

  if (strcmp(type, "pr_merged") == 0)        strncpy(notifLabel, "PR MERGED!",   21);
  else if (strcmp(type, "pr_approved") == 0)  strncpy(notifLabel, "PR APPROVED!", 21);
  else if (strcmp(type, "pr_comment") == 0)   strncpy(notifLabel, "PR Comment",   21);
  else                                         strncpy(notifLabel, "New Commit!",  21);
  notifLabel[21] = '\0';

  strncpy(notifMsg, msg, 50);   notifMsg[50] = '\0';
  strncpy(notifAuthor, author, 21); notifAuthor[21] = '\0';

  clearAmbient();
  if (notifPositive) {
    setMoodTracked(HAPPY);
    roboEyes.setVFlicker(true, 5);
  } else {
    setMoodTracked(TIRED);
    roboEyes.setHFlicker(true, 20);
  }

  audioRequest = notifPositive ? 1 : -1;  // wake audioTask
  Serial.printf("[NOTIF] phase0: %s — %s\n", notifLabel, notifMsg);
}

// -------- GITHUB POLL (runs in pollTask on Core 0) --------
void checkGitHubNotifications() {
  WiFiClientSecure client;
  client.setInsecure(); // skip cert verification — replace with setCACert() for production

  HTTPClient http;
  String url = String(FIREBASE_DB_URL) + "/notifications/latest.json";

  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[POLL] HTTP %d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok || doc.isNull()) return;

  unsigned long ts = doc["timestamp"].as<unsigned long>();
  if (ts <= lastSeenTimestamp) return;
  lastSeenTimestamp = ts;

  const char* type   = doc["type"]    | "commit";
  const char* msg    = doc["message"] | "New activity";
  const char* author = doc["author"]  | "unknown";

  if (xSemaphoreTake(stateMux, portMAX_DELAY)) {
    triggerNotification(type, msg, author);
    xSemaphoreGive(stateMux);
  }
}

void pollTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(15000)); // wait for WiFi + NTP to stabilise after boot
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      checkGitHubNotifications();
    }
    vTaskDelay(pdMS_TO_TICKS(GITHUB_POLL_INTERVAL_MS));
  }
}


// -------- WEB ROUTES --------

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Desk Buddy</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: sans-serif; background: #111; color: #eee; text-align: center; padding: 20px; }
    h2 { margin-bottom: 24px; font-size: 1.4em; letter-spacing: 1px; }
    h3 { margin: 20px 0 10px; font-size: 0.8em; color: #888; text-transform: uppercase; letter-spacing: 2px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; max-width: 320px; margin: 0 auto 10px; }
    .full { max-width: 320px; margin: 0 auto 20px; width: 100%; display: block; }
    button {
      padding: 14px 8px; font-size: 14px; border: 2px solid #333;
      border-radius: 12px; cursor: pointer; background: #1a1a1a;
      color: #ccc; transition: all 0.15s;
    }
    button:active { background: #333; }
    button.active { background: #0af; border-color: #0af; color: #000; font-weight: bold; }
    #pomTimer { font-size: 3em; margin: 10px 0 4px; font-variant-numeric: tabular-nums; letter-spacing: 2px; }
    #pomLabel { font-size: 0.85em; color: #888; margin-bottom: 14px; min-height: 1.2em; }
    #pomBar { width: 100%; max-width: 320px; height: 6px; background: #222; border-radius: 3px; margin: 0 auto 16px; overflow: hidden; }
    #pomFill { height: 100%; width: 0%; background: #0af; border-radius: 3px; transition: width 1s linear; }
    #petBtn {
      width: 100%; max-width: 320px; padding: 18px; font-size: 1.2em;
      background: #1a1a1a; border: 2px dashed #555; border-radius: 16px;
      cursor: pointer; margin: 0 auto; display: block; transition: all 0.1s;
    }
    #petBtn:active { background: #2a2a2a; transform: scale(0.97); }
    #petFeedback { min-height: 1.4em; font-size: 0.85em; color: #0af; margin-top: 6px; }
  </style>
</head>
<body>
  <h2>Desk Buddy</h2>

  <h3>Pet Me</h3>
  <button id="petBtn" onclick="petTap()">Pat Head</button>
  <div id="petFeedback"></div>

  <h3>Clock</h3>
  <div class="grid">
    <button onclick="fetch('/clock')">Clock</button>
    <button onclick="fetch('/eyes')">Eyes</button>
    <button onclick="fetch('/sleep')" style="grid-column:span 2;">Sleep</button>
  </div>

  <h3>Pomodoro</h3>
  <div id="pomTimer">25:00</div>
  <div id="pomLabel">Ready to focus</div>
  <div id="pomBar"><div id="pomFill"></div></div>
  <div class="grid">
    <button id="btn-pom-start" onclick="pomStart()">Start Focus</button>
    <button onclick="pomReset()">Reset</button>
  </div>

  <h3>Mood</h3>
  <div class="grid">
    <button id="mood-happy"   onclick="setMood('happy')">😊 Happy</button>
    <button id="mood-tired"   onclick="setMood('tired')">😴 Tired</button>
    <button id="mood-angry"   onclick="setMood('angry')">😠 Angry</button>
    <button id="mood-default" onclick="setMood('default')">😐 Default</button>
    <button id="mood-roam" onclick="setRoam()" style="grid-column:span 2;">👀 Roam</button>
  </div>

<script>
  // ---- Pet ----
  let tapCount = 0, tapTimer = null;
  const petLabels = ['', 'Happy!', 'Haha!!', 'Hey!! Stop!!'];

  function petTap() {
    tapCount++;
    document.getElementById('petFeedback').textContent = tapCount >= 3 ? petLabels[3] : petLabels[tapCount];
    clearTimeout(tapTimer);
    tapTimer = setTimeout(() => {
      fetch('/pet?taps=' + tapCount);
      tapCount = 0;
      setTimeout(() => { document.getElementById('petFeedback').textContent = ''; }, 2000);
    }, 400);
  }

  // ---- Mood ----
  function setMood(m) {
    fetch('/mood?v=' + m);
    document.querySelectorAll('[id^=mood-]').forEach(b => b.classList.remove('active'));
    document.getElementById('mood-' + m).classList.add('active');
  }

  function setRoam() {
    fetch('/roam');
    document.querySelectorAll('[id^=mood-]').forEach(b => b.classList.remove('active'));
    document.getElementById('mood-roam').classList.add('active');
  }

  // ---- Pomodoro ----
  let pomInterval = null, pomTotal = 0, pomRemaining = 0, pomPhase = 'idle';

  function pomStart() {
    fetch('/pomodoro?v=start');
    pomPhase = 'focus'; pomTotal = 25 * 60; pomRemaining = pomTotal;
    document.getElementById('btn-pom-start').classList.add('active');
    tick(); clearInterval(pomInterval); pomInterval = setInterval(tick, 1000);
  }

  function pomReset() {
    fetch('/pomodoro?v=reset');
    clearInterval(pomInterval); pomInterval = null; pomPhase = 'idle';
    document.getElementById('pomTimer').textContent = '25:00';
    document.getElementById('pomLabel').textContent = 'Ready to focus';
    document.getElementById('pomFill').style.width = '0%';
    document.getElementById('btn-pom-start').classList.remove('active');
  }

  function tick() {
    pomRemaining--;
    if (pomRemaining <= 0) {
      if (pomPhase === 'focus') {
        pomPhase = 'break'; pomTotal = 5 * 60; pomRemaining = pomTotal;
        document.getElementById('pomLabel').textContent = 'Break time!';
        document.getElementById('pomFill').style.background = '#4f4';
      } else if (pomPhase === 'break') {
        pomPhase = 'overtime'; clearInterval(pomInterval);
        document.getElementById('pomTimer').textContent = '00:00';
        document.getElementById('pomLabel').textContent = 'Overtime - take a break!';
        document.getElementById('pomFill').style.width = '100%';
        document.getElementById('pomFill').style.background = '#f44';
        return;
      }
    }
    const m = String(Math.floor(pomRemaining / 60)).padStart(2, '0');
    const s = String(pomRemaining % 60).padStart(2, '0');
    document.getElementById('pomTimer').textContent = m + ':' + s;
    document.getElementById('pomFill').style.width = ((pomTotal - pomRemaining) / pomTotal * 100).toFixed(1) + '%';
    if (pomPhase === 'focus') document.getElementById('pomLabel').textContent = 'Stay focused';
  }
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

void handlePet() {
  int taps = server.arg("taps").toInt();
  if (taps < 1)  taps = 1;
  if (taps > 10) taps = 10;
  enterPetMode(taps);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String v = server.arg("v");
  autoSleepActive = false;
  clearAmbient();
  if (v == "auto") {
    currentMode  = MODE_AUTO;
    lastAutoMood = -1;
    applyAmbient();
  } else {
    currentMode = MODE_STATUS;
    if (v == "available") {
      setMoodTracked(HAPPY);
      applyAmbient();
    } else if (v == "busy") {
      setMoodTracked(ANGRY);
      roboEyes.setAutoblinker(ON, 6, 0);
    } else if (v == "meeting") {
      setMoodTracked(DEFAULT);
      roboEyes.setCuriosity(true);
      roboEyes.setIdleMode(ON, 1, 1);
      roboEyes.setAutoblinker(ON, 4, 2);
    } else if (v == "away") {
      setMoodTracked(TIRED);
      roboEyes.setAutoblinker(ON, 7, 3);
    }
  }
  server.send(200, "text/plain", "OK");
}

void handlePomodoro() {
  String v = server.arg("v");
  if (v == "start") {
    currentMode       = MODE_POMODORO_FOCUS;
    pomodoroStartTime = millis();
    clearAmbient();
    setMoodTracked(ANGRY);
    roboEyes.setPosition(S);
    roboEyes.setAutoblinker(ON, 8, 0);
  } else if (v == "reset") {
    currentMode  = MODE_AUTO;
    lastAutoMood = -1;
    clearAmbient();
    applyAmbient();
  }
  server.send(200, "text/plain", "OK");
}

void handleClock() {
  currentMode = MODE_CLOCK;
  server.send(200, "text/plain", "OK");
}

void handleEyes() {
  currentMode     = MODE_AUTO;
  lastAutoMood    = -1;
  autoSleepActive = false;
  clearAmbient();
  applyAmbient();
  server.send(200, "text/plain", "OK");
}

void handleSleep() {
  enterSleep();
  server.send(200, "text/plain", "OK");
}

void handleTouch2() {
  String action = server.arg("action");

  if (action == "tap") {
    if (currentMode == MODE_CLOCK) {
      // Clock -> Pomodoro start
      currentMode       = MODE_POMODORO_FOCUS;
      pomodoroStartTime = millis();
      clearAmbient();
      setMoodTracked(ANGRY);
      roboEyes.setPosition(S);
      roboEyes.setAutoblinker(ON, 8, 0);
    } else if (currentMode == MODE_POMODORO_FOCUS ||
               currentMode == MODE_POMODORO_BREAK  ||
               currentMode == MODE_POMODORO_OVERTIME) {
      // Pomodoro -> Eyes (auto)
      currentMode     = MODE_AUTO;
      lastAutoMood    = -1;
      autoSleepActive = false;
      clearAmbient();
      applyAmbient();
    } else {
      // Anything else -> Clock
      currentMode = MODE_CLOCK;
    }
  } else if (action == "longpress") {
    if (currentMode == MODE_POMODORO_FOCUS ||
        currentMode == MODE_POMODORO_BREAK  ||
        currentMode == MODE_POMODORO_OVERTIME) {
      currentMode     = MODE_AUTO;
      lastAutoMood    = -1;
      autoSleepActive = false;
      clearAmbient();
      applyAmbient();
    }
  }
  server.send(200, "text/plain", "OK");
}



void handleMood() {
  String v = server.arg("v");
  int mood = DEFAULT;
  if      (v == "happy")   mood = HAPPY;
  else if (v == "tired")   mood = TIRED;
  else if (v == "angry")   mood = ANGRY;
  else if (v == "default") mood = DEFAULT;
  currentMode = MODE_STATUS;
  clearAmbient();
  setMoodTracked(mood);
  roboEyes.setPosition(DEFAULT);
  server.send(200, "text/plain", "OK");
}

void handleRoam() {
  applyAmbient();
  server.send(200, "text/plain", "OK");
}

// -------- WIFI TASK (Core 0) --------

void wifiTask(void* pv) {
  for (;;) {
    if (xSemaphoreTake(stateMux, portMAX_DELAY)) {
      server.handleClient();
      xSemaphoreGive(stateMux);
    }
    vTaskDelay(1); // yield 1 tick so Core 1 can acquire the mutex
  }
}

// -------- SETUP --------

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  Wire.begin(21, 22);
  Serial.begin(115200);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 failed");
    while (true);
  }

  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 30); // 30fps leaves headroom for overlay flush
  roboEyes.autoFlush = false; // we handle display() ourselves

  // Overlay occupies y=0..9 (10px). Re-center eyes in the remaining y=10..63 (54px).
  // Centered y = 10 + (54 - 36) / 2 = 19. Default was (64 - 36) / 2 = 14. Delta = 5.
  const int Y_SHIFT = 5;
  roboEyes.eyeLyDefault += Y_SHIFT;
  roboEyes.eyeLy        += Y_SHIFT;
  roboEyes.eyeLyNext    += Y_SHIFT;
  roboEyes.eyeRy        += Y_SHIFT;
  roboEyes.eyeRyNext    += Y_SHIFT;
  setMoodTracked(TIRED);
  applyAmbient();

  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  configTime(19800, 0, "pool.ntp.org");

  server.on("/",         handleRoot);
  server.on("/pet",      handlePet);
  server.on("/status",   handleStatus);
  server.on("/pomodoro", handlePomodoro);
  server.on("/clock",    handleClock);
  server.on("/eyes",     handleEyes);
  server.on("/sleep",    handleSleep);
  server.on("/mood",     handleMood);
  server.on("/roam",     handleRoam);
  server.on("/touch2",   handleTouch2);
  server.begin();

  stateMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(wifiTask,  "wifi",  4096, NULL, 1, NULL, 0); // Core 0
  xTaskCreatePinnedToCore(pollTask,  "poll",  8192, NULL, 1, NULL, 0); // Core 0
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 1, NULL, 0); // Core 0

  setupSpeaker();
  playBootSound();

  lastActivityMs = millis();
  roboEyes.anim_laugh();
}


// -------- MAX98357A SPEAKER --------

void setupSpeaker() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SPK_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE, // MAX98357A doesn't need MCLK
    .bck_io_num   = SPK_BCLK,
    .ws_io_num    = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  // Serial.println("[SPK] MAX98357A initialised");
}

// Plays a sine wave tone at `freq` Hz for `durationMs` ms at SPK_VOLUME.
void playTone(int freq, int durationMs) {
  const int amplitude = (int)(32767 * SPK_VOLUME);
  const int totalSamples = SPK_SAMPLE_RATE * durationMs / 1000;
  const int chunkFrames = 64;
  int16_t buf[chunkFrames * 2]; // stereo

  for (int i = 0; i < totalSamples; i += chunkFrames) {
    int frames = min(chunkFrames, totalSamples - i);
    for (int j = 0; j < frames; j++) {
      float t = (float)(i + j) / SPK_SAMPLE_RATE;
      int16_t s = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * freq * t));
      buf[j * 2]     = s; // left
      buf[j * 2 + 1] = s; // right
    }
    size_t written;
    i2s_write(SPK_PORT, buf, frames * 4, &written, portMAX_DELAY);
  }
}

// Positive: ascending 3-note chime (commit / PR merged / PR approved)
void playPositiveChime() {
  playTone(659,  120);  // E5
  playTone(880,  120);  // A5
  playTone(1047, 200);  // C6
}

// Negative: descending 2-note tone (PR comment)
void playNegativeChime() {
  playTone(523, 180);  // C5
  playTone(392, 250);  // G4
}

// Robot smile: wobbling frequency-modulated chirps — R2-D2 happy style
// freq(t) = baseFreq + wobbleDepth * sin(2π * wobbleRate * t)
void playWobbleTone(int baseFreq, int wobbleDepth, float wobbleRate, int durationMs) {
  const int amplitude   = (int)(32767 * SPK_VOLUME);
  const int totalSamples = SPK_SAMPLE_RATE * durationMs / 1000;
  const int chunkFrames  = 64;
  int16_t buf[chunkFrames * 2];

  for (int i = 0; i < totalSamples; i += chunkFrames) {
    int frames = min(chunkFrames, totalSamples - i);
    for (int j = 0; j < frames; j++) {
      float t    = (float)(i + j) / SPK_SAMPLE_RATE;
      float freq = baseFreq + wobbleDepth * sinf(2.0f * (float)M_PI * wobbleRate * t);
      // Integrate phase properly to avoid clicks at wobble boundary
      float phase = 2.0f * (float)M_PI * (baseFreq * t
                    + (wobbleDepth / wobbleRate) * (1.0f - cosf(2.0f * (float)M_PI * wobbleRate * t)) / (2.0f * (float)M_PI));
      int16_t s = (int16_t)(amplitude * sinf(phase));
      buf[j * 2]     = s;
      buf[j * 2 + 1] = s;
    }
    size_t written;
    i2s_write(SPK_PORT, buf, frames * 4, &written, portMAX_DELAY);
  }
}

void playRobotSmile() {
  playWobbleTone(600,  120, 18.0f, 140);  // low warble
  playWobbleTone(900,  100, 22.0f, 130);  // mid warble
  playWobbleTone(1200,  80, 28.0f, 160);  // high chirp
  playWobbleTone(1500,  60, 35.0f, 120);  // bright finish
}

// Angry: rapid wobble while eye H-flickers, then low harsh tones
void playAngrySound() {
  playWobbleTone(300, 180, 25.0f, 200);  // aggressive low wobble (matches H-flicker)
  playWobbleTone(250, 200, 30.0f, 180);  // deeper wobble
  playTone(180, 200);                     // heavy low growl
  playTone(150, 300);                     // deeper angry tone
}

// Pomodoro focus ended → break time: descending bell tones
void playPomodoroFocusEnd() {
  playTone(1047, 150);  // C6
  playTone(880,  150);  // A5
  playTone(784,  150);  // G5
  playTone(659,  300);  // E5 — settle
}

// Pomodoro break ended → overtime: urgent ascending alarm
void playPomodoroBreakEnd() {
  for (int i = 0; i < 3; i++) {
    playTone(880,  120);
    playTone(1047, 120);
  }
  playTone(1319, 300);  // E6 — final sharp note
}

// Boot jingle: cheerful rising fanfare
void playBootSound() {
  playTone(523,  80);   // C5
  playTone(659,  80);   // E5
  playTone(784,  80);   // G5
  playWobbleTone(1047, 60, 12.0f, 250);  // C6 with shimmer
}

void audioTask(void* pv) {
  for (;;) {
    int8_t req = audioRequest;
    if (req != 0) {
      audioRequest = 0;
      if      (req ==  1) playPositiveChime();
      else if (req == -1) playNegativeChime();
      else if (req ==  2) playRobotSmile();
      else if (req ==  3) playAngrySound();
      else if (req ==  4) playPomodoroFocusEnd();
      else if (req ==  5) playPomodoroBreakEnd();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// -------- INACTIVITY SLEEP --------

void handleInactivitySleep() {
  if (currentMode == MODE_SLEEP || currentMode == MODE_SLEEP_TRANSITION) return;
  if (autoSleepActive) return;
  if (inPomodoroMode()) return;
  if (millis() - lastActivityMs < INACTIVITY_SLEEP_MS) return;

  enterSleep();
  Serial.println("[SLEEP] Inactivity timeout");
}

// -------- AUTO SLEEP --------

void handleAutoSleep() {
  if (currentMode != MODE_AUTO && !((currentMode == MODE_SLEEP || currentMode == MODE_SLEEP_TRANSITION) && autoSleepActive)) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int h = timeinfo.tm_hour;
  bool sleepTime = (h >= 21 || h < 7);

  if (sleepTime && !autoSleepActive) {
    autoSleepActive = true;
    enterSleep();
  } else if (!sleepTime && autoSleepActive) {
    autoSleepActive = false;
    currentMode = MODE_AUTO;
    lastAutoMood = -1;
    applyAmbient();
  }
}

// -------- PET STATE MACHINE --------

void handlePetStateMachine() {
  if (currentMode != MODE_PET) return;

  if (petAction == PET_HAPPY) {
    if (millis() >= petTimer1) restoreFromPet();
  } else if (petAction == PET_LAUGH) {
    if (millis() >= petTimer1) { roboEyes.setVFlicker(false); restoreFromPet(); }
  } else if (petAction == PET_CONFUSED) {
    if (petPhase == 0 && millis() >= petTimer1) {
      roboEyes.setHFlicker(false);
      setMoodTracked(ANGRY);
      petTimer2 = millis() + 3000;
      petPhase = 1;
    } else if (petPhase == 1 && millis() >= petTimer2) {
      restoreFromPet();
    }
  }
}

// -------- POMODORO STATE MACHINE --------

void handlePomodoroStateMachine() {
  if (currentMode != MODE_POMODORO_FOCUS &&
      currentMode != MODE_POMODORO_BREAK  &&
      currentMode != MODE_POMODORO_OVERTIME) return;

  unsigned long elapsed = millis() - pomodoroStartTime;

  if (currentMode == MODE_POMODORO_FOCUS && elapsed >= POMODORO_FOCUS_MS) {
    currentMode       = MODE_POMODORO_BREAK;
    pomodoroStartTime = millis();
    setMoodTracked(HAPPY);
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.anim_laugh();
    audioRequest = 4;  // focus-end bell
  } else if (currentMode == MODE_POMODORO_BREAK && elapsed >= POMODORO_BREAK_MS) {
    currentMode = MODE_POMODORO_OVERTIME;
    setMoodTracked(TIRED);
    roboEyes.setSweat(true);
    audioRequest = 5;  // break-end alarm
  }
}

// -------- AUTO MOOD REFRESH --------

void handleAutoMood() {
  if (currentMode != MODE_AUTO) return;

  int mood = getTimeBasedMood();
  if (mood != lastAutoMood) {
    setMoodTracked(mood);
    lastAutoMood = mood;
  }
}

// -------- DISPLAY RENDER --------

void handleDisplayRender() {
  bool timeForFrame = (millis() - roboEyes.fpsTimer >= roboEyes.frameInterval);
  roboEyes.update();
  if (timeForFrame) {
    drawPomOverlay();
    display.display();
  }
}

// -------- LOOP (Core 1) --------

void loop() {
  xSemaphoreTake(stateMux, portMAX_DELAY);

  handleTouchSensor();

  // ---- Notification 3-phase state machine ----
  // Phase 0 (2s): eyes animate with mood reaction
  // Phase 1 (3.5s): full-screen text, Pomodoro keeps ticking in background
  // Phase 2 (2.5s): eyes animate again, then restore previous mode
  if (currentMode == MODE_NOTIFICATION) {
    unsigned long now = millis();

    if (notifPhase == 0) {
      bool timeForFrame = (now - roboEyes.fpsTimer >= roboEyes.frameInterval);
      roboEyes.update();
      if (timeForFrame) display.display();

      if (now >= notifPhaseTimer) {
        roboEyes.setVFlicker(false);
        roboEyes.setHFlicker(false);
        notifPhase = 1;
        notifPhaseTimer = now + NOTIF_PHASE1_MS;
        Serial.println("[NOTIF] phase1: fullscreen");
      }

    } else if (notifPhase == 1) {
      drawNotifFullScreen();

      if (now >= notifPhaseTimer) {
        notifPhase = 2;
        notifPhaseTimer = now + NOTIF_PHASE2_MS;
        if (notifPositive) {
          setMoodTracked(HAPPY);
          roboEyes.setVFlicker(true, 5);
        } else {
          setMoodTracked(TIRED);
          roboEyes.setHFlicker(true, 20);
        }
        Serial.println("[NOTIF] phase2: eyes again");
      }

    } else {
      bool timeForFrame = (now - roboEyes.fpsTimer >= roboEyes.frameInterval);
      roboEyes.update();
      if (timeForFrame) display.display();

      if (now >= notifPhaseTimer) {
        roboEyes.setVFlicker(false);
        roboEyes.setHFlicker(false);
        currentMode = preNotifMode;
        setMoodTracked(preNotifMood);
        if (preNotifAmbient) applyAmbient();
        if (preNotifMode == MODE_AUTO) lastAutoMood = -1;
        Serial.println("[NOTIF] restored");
      }
    }

    xSemaphoreGive(stateMux);
    return;
  }

  if (currentMode == MODE_CLOCK) {
    showTime();
    xSemaphoreGive(stateMux);
    return;
  }

  handleAutoSleep();

  if (currentMode == MODE_SLEEP_TRANSITION) {
    float t = (float)(millis() - sleepTransStart) / (float)SLEEP_TRANSITION_MS;
    if (t >= 1.0f) {
      roboEyes.eyeLheightCurrent = roboEyes.eyeLheightDefault;
      roboEyes.eyeRheightCurrent = roboEyes.eyeRheightDefault;
      roboEyes.eyeLheightNext    = roboEyes.eyeLheightDefault;
      roboEyes.eyeRheightNext    = roboEyes.eyeRheightDefault;
      currentMode = MODE_SLEEP;
    } else {
      int h = max(1, (int)(roboEyes.eyeLheightDefault * (1.0f - t)));
      roboEyes.eyeLheightCurrent = h;
      roboEyes.eyeRheightCurrent = h;
      roboEyes.eyeLheightNext    = h;
      roboEyes.eyeRheightNext    = h;
      bool timeForFrame = (millis() - roboEyes.fpsTimer >= roboEyes.frameInterval);
      roboEyes.update();
      if (timeForFrame) display.display();
    }
    xSemaphoreGive(stateMux);
    return;
  }

  if (currentMode == MODE_SLEEP) {
    drawSleepFace();
    xSemaphoreGive(stateMux);
    return;
  }

  handleInactivitySleep();
  handlePetStateMachine();
  handlePomodoroStateMachine();
  handleAutoMood();
  handleDisplayRender();

  xSemaphoreGive(stateMux);
}
