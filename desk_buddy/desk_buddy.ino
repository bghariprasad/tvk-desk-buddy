#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

const char* ssid     = "Veedu";
const char* password = "Password@123";

WebServer server(80);

#define TOUCH_PIN 4

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// -------- MODES --------
enum Mode {
  MODE_AUTO,
  MODE_STATUS,
  MODE_POMODORO_FOCUS,
  MODE_POMODORO_BREAK,
  MODE_POMODORO_OVERTIME,
  MODE_CLOCK,
  MODE_PET,
  MODE_SLEEP
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

// -------- PHYSICAL TOUCH TAP --------
int           touchTapCount  = 0;
bool          touchLastState = false;
unsigned long touchLastTap   = 0;
#define TOUCH_WINDOW_MS 400

// Single-tap cycles through: Clock → Pomodoro → Eyes (auto)
const Mode TAP_CYCLE[] = { MODE_CLOCK, MODE_POMODORO_FOCUS, MODE_AUTO };
const int  TAP_CYCLE_LEN = 3;
int        tapCycleIndex = -1;

void cycleTapMode() {
  tapCycleIndex = (tapCycleIndex + 1) % TAP_CYCLE_LEN;
  Mode next = TAP_CYCLE[tapCycleIndex];

  if (next == MODE_CLOCK) {
    currentMode = MODE_CLOCK;
    clearAmbient();
  } else if (next == MODE_POMODORO_FOCUS) {
    currentMode       = MODE_POMODORO_FOCUS;
    pomodoroStartTime = millis();
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

void restoreFromPet() {
  petAction   = PET_NONE;
  petPhase    = 0;
  currentMode = prePetMode;
  roboEyes.setVFlicker(false);
  roboEyes.setHFlicker(false);
  roboEyes.setSweat(false);
  setMoodTracked(prePetMood);
  if (prePetAmbient) applyAmbient();
  if (prePetMode == MODE_AUTO) lastAutoMood = -1;
}

void enterPetMode(int taps) {
  if (currentMode == MODE_PET) return; // ignore taps during active reaction

  if (taps == 1) { cycleTapMode(); return; }

  prePetMode    = currentMode;
  prePetMood    = trackedMood;
  prePetAmbient = ambientActive;
  currentMode   = MODE_PET;
  clearAmbient();

  if (taps == 2) {
    petAction = PET_LAUGH;
    setMoodTracked(HAPPY);
    roboEyes.setVFlicker(true, 5);
    petTimer1 = millis() + 3000;
  } else {
    petAction = PET_CONFUSED;
    petPhase  = 0;
    roboEyes.setHFlicker(true, 20);
    petTimer1 = millis() + 2000;
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
  if (currentMode != MODE_POMODORO_FOCUS &&
      currentMode != MODE_POMODORO_BREAK &&
      currentMode != MODE_POMODORO_OVERTIME) return;

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (currentMode == MODE_POMODORO_OVERTIME) {
    display.setCursor(15, 1);
    display.print("DONE! - REST NOW");
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
    #t2Btn {
      width: 100%; max-width: 320px; padding: 18px; font-size: 1.2em;
      background: #1a1a1a; border: 2px dashed #555; border-radius: 16px;
      cursor: pointer; margin: 0 auto; display: block; transition: all 0.1s; user-select: none;
    }
    #t2Btn:active { background: #2a2a2a; transform: scale(0.97); }
    #t2Btn.holding { border-color: #f80; background: #221500; }
    #t2Feedback { min-height: 1.4em; font-size: 0.85em; color: #0af; margin-top: 6px; }
  </style>
</head>
<body>
  <h2>Desk Buddy</h2>

  <h3>Pet Me</h3>
  <button id="petBtn" onclick="petTap()">Pat Head</button>
  <div id="petFeedback"></div>

  <h3>Touch 2</h3>
  <button id="t2Btn"
    onmousedown="t2Start()" ontouchstart="t2Start(event)"
    onmouseup="t2End()"     ontouchend="t2End(event)"
    onmouseleave="t2Cancel()">Hold or Tap</button>
  <div id="t2Feedback"></div>

  <h3>Status</h3>
  <div class="grid">
    <button id="btn-available" onclick="setStatus('available')">Available</button>
    <button id="btn-busy"      onclick="setStatus('busy')">Busy</button>
    <button id="btn-meeting"   onclick="setStatus('meeting')">In Meeting</button>
    <button id="btn-away"      onclick="setStatus('away')">Away</button>
    <button id="btn-sleep"     onclick="setSleep()">Sleep</button>
    <button class="full" id="btn-auto" onclick="setStatus('auto')" style="grid-column:span 2;">Auto Mode</button>
  </div>

  <h3>Pomodoro</h3>
  <div id="pomTimer">25:00</div>
  <div id="pomLabel">Ready to focus</div>
  <div id="pomBar"><div id="pomFill"></div></div>
  <div class="grid">
    <button id="btn-pom-start" onclick="pomStart()">Start Focus</button>
    <button onclick="pomReset()">Reset</button>
  </div>

  <h3>Display</h3>
  <div class="grid" style="margin-top:10px;">
    <button onclick="fetch('/clock')">Clock</button>
    <button onclick="fetch('/eyes')">Eyes</button>
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

  // ---- Status ----
  function setStatus(s) {
    fetch('/status?v=' + s);
    document.querySelectorAll('button[id^=btn-]').forEach(b => b.classList.remove('active'));
    const el = document.getElementById('btn-' + s);
    if (el) el.classList.add('active');
  }

  function setSleep() {
    fetch('/sleep');
    document.querySelectorAll('button[id^=btn-]').forEach(b => b.classList.remove('active'));
    document.getElementById('btn-sleep').classList.add('active');
  }

  // ---- Touch 2 ----
  let t2Timer = null, t2Long = false;
  const T2_HOLD = 800;

  function t2Start(e) { if(e) e.preventDefault();
    t2Long = false;
    document.getElementById('t2Btn').classList.add('holding');
    t2Timer = setTimeout(() => {
      t2Long = true;
      document.getElementById('t2Btn').classList.remove('holding');
      fetch('/touch2?action=longpress');
      const fb = document.getElementById('t2Feedback');
      fb.textContent = 'Pomodoro reset!';
      setTimeout(() => fb.textContent = '', 2000);
    }, T2_HOLD);
  }

  function t2End(e) { if(e) e.preventDefault();
    clearTimeout(t2Timer);
    document.getElementById('t2Btn').classList.remove('holding');
    if (!t2Long) {
      fetch('/touch2?action=tap');
      const fb = document.getElementById('t2Feedback');
      fb.textContent = 'Switched!';
      setTimeout(() => fb.textContent = '', 1000);
    }
    t2Long = false;
  }

  function t2Cancel() { clearTimeout(t2Timer); document.getElementById('t2Btn').classList.remove('holding'); t2Long = false; }

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
  currentMode = MODE_SLEEP;
  clearAmbient();
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
  server.on("/touch2",   handleTouch2);
  server.begin();

  roboEyes.anim_laugh();
}


// -------- AUTO SLEEP --------

void handleAutoSleep() {
  if (currentMode != MODE_AUTO && !(currentMode == MODE_SLEEP && autoSleepActive)) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int h = timeinfo.tm_hour;
  bool sleepTime = (h >= 21 || h < 7);

  if (sleepTime && !autoSleepActive) {
    autoSleepActive = true;
    currentMode = MODE_SLEEP;
    clearAmbient();
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
  } else if (currentMode == MODE_POMODORO_BREAK && elapsed >= POMODORO_BREAK_MS) {
    currentMode = MODE_POMODORO_OVERTIME;
    setMoodTracked(TIRED);
    roboEyes.setSweat(true);
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

// -------- LOOP --------

void loop() {
  server.handleClient();
  handleTouchSensor();

  if (currentMode == MODE_CLOCK) { showTime(); return; }

  handleAutoSleep();

  if (currentMode == MODE_SLEEP) { drawSleepFace(); return; }

  handlePetStateMachine();
  handlePomodoroStateMachine();
  handleAutoMood();
  handleDisplayRender();
}
