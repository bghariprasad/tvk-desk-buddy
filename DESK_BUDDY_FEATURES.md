# TVK Desk Buddy — Feature Reference

## Hardware
| Component | Detail |
|---|---|
| Board | ESP32 |
| Display | SSD1306 OLED 128×64px, I2C (SDA=21, SCL=22, addr=0x3C) |
| Eyes library | FluxGarage RoboEyes |
| Connectivity | WiFi (WebServer on port 80), HTTPS polling (Firebase RTDB) |
| Speaker amp | MAX98357A I2S mono amplifier |
| Speaker | 8Ω 0.5W |

### Pin Map
| Pin | Role | Type |
|---|---|---|
| GPIO 4 | Touch sensor (tap input) | Digital Input |
| GPIO 21 | I2C SDA — OLED display | I2C |
| GPIO 22 | I2C SCL — OLED display | I2C |
| GPIO 26 | MAX98357A BCLK — I2S bit clock | I2S (SPK) |
| GPIO 27 | MAX98357A LRC — I2S LR clock | I2S (SPK) |
| GPIO 33 | MAX98357A DIN — I2S data out | I2S (SPK) |
| 3.3V | MAX98357A SD — amp enable | Power |
| 5V | MAX98357A VIN — amp power | Power |
| GND | MAX98357A GND | Power |

### MAX98357A Wiring
| MAX98357A Pin | ESP32 | Notes |
|---|---|---|
| VIN | 5V (VIN) | Use 5V — not 3.3V |
| GND | GND | — |
| BCLK | GPIO 26 | I2S bit clock |
| LRC | GPIO 27 | I2S left/right clock |
| DIN | GPIO 33 | I2S data |
| SD | 3.3V | Pull HIGH to keep amp enabled |
| GAIN | (unconnected) | Floating = 9 dB |
| OUT+ | Speaker + | — |
| OUT− | Speaker − | — |

> Software volume is set to **40%** (`SPK_VOLUME = 0.40f`) to protect the 0.5W speaker.

### Speaker Config
| Parameter | Value |
|---|---|
| I2S port | I2S_NUM_1 |
| Sample rate | 44100 Hz |
| Bit depth | 16-bit |
| Channel | Stereo output (mono source duplicated to both channels) |
| DMA buffers | 8 × 64 samples |
| Volume | 40% (`SPK_VOLUME = 0.40f`) |

---

## Modes

| Mode | Description |
|---|---|
| `MODE_AUTO` | Time-based mood, ambient personality active |
| `MODE_STATUS` | Manually set mood (available / busy / meeting / away) |
| `MODE_CLOCK` | Full-screen digital clock, eye animation paused |
| `MODE_POMODORO_READY` | Pomodoro loaded, timer not yet started |
| `MODE_POMODORO_FOCUS` | Focus session running |
| `MODE_POMODORO_BREAK` | Break session running |
| `MODE_POMODORO_OVERTIME` | Break ended, return-to-work prompt |
| `MODE_PET` | Temporary reaction from tap input |
| `MODE_SLEEP` | Sleep face shown (auto or inactivity) |
| `MODE_NOTIFICATION` | 3-phase GitHub notification overlay |

---

## Feature 1 — Time-Aware Mood (Auto Mode)

The buddy automatically changes its eye expression based on the time of day.

| Time | Mood |
|---|---|
| 07:00 – 08:59 | Tired |
| 09:00 – 11:59 | Default |
| 12:00 – 17:59 | Default |
| 18:00 – 20:59 | Happy |
| 21:00 – 06:59 | Sleep (auto sleep kicks in) |

> Time is synced via NTP (`pool.ntp.org`) on boot. Timezone: IST (UTC+5:30).

---

## Feature 2 — Sleep

Two independent mechanisms put the buddy to sleep.

### Auto Sleep (time-based)
Triggers when `MODE_AUTO` is active and the hour is **21:00 – 06:59**. Wakes automatically when the hour exits that range.

### Inactivity Sleep
Triggers after **10 minutes** of no touch input in any mode except Pomodoro. Any tap wakes the device and restores `MODE_STATUS` (In Meeting appearance).

### Sleep Face
| Element | Detail |
|---|---|
| Eyes | Two thin horizontal dashed bars |
| Zzz animation | Two floating `z` characters drifting upward, cycling every 600 ms |

---

## Feature 3 — Pomodoro Timer

A focus/break cycle timer visible on both the OLED and the web UI.

### OLED Overlay (top strip, y=0–9px)
| Element | Position | Detail |
|---|---|---|
| Label + time | y=1, centered | `FOCUS 25:00` or `BREAK 05:00` |
| Progress bar | y=9, 1px tall | Fills left→right as session progresses |

### Eye Behaviour During Pomodoro
| Phase | Eye mood | Eye position | Extra |
|---|---|---|---|
| Ready | Angry | Center-bottom (S) | Infrequent blink (8s) |
| Focus | Angry | Center-bottom (S) | Infrequent blink (8s) |
| Break | Happy | Resets to center | Laugh animation + focus-end chime |
| Overtime | Tired | — | Sweat drops + alarm sound |

### Web UI Timer
| Phase | Bar colour | Label |
|---|---|---|
| Focus | Blue | Stay focused |
| Break | Green | Break time! |
| Overtime | Red | Overtime - take a break! |

### Durations
| Phase | Duration |
|---|---|
| Focus | 25 seconds *(test value — change to `25UL * 60 * 1000` for production)* |
| Break | 5 seconds *(test value — change to `5UL * 60 * 1000` for production)* |

---

## Feature 4 — Status Indicator

Manually set availability. Held until changed or Auto Mode is re-selected.

| Status | Eye mood | Ambient | Blink interval |
|---|---|---|---|
| Available | Happy | On (curiosity + idle wander) | 3s ±2s |
| Busy | Angry | Off | 6s (fixed) |
| In Meeting | Default | Curious + idle (fast) | 4s ±2s |
| Away | Tired | Off | 7s ±3s |
| Auto Mode | Time-based (see Feature 1) | On | 3s ±2s |

---

## Feature 5 — Ambient Personality

When active, eyes wander, glance sideways, and blink naturally. Active during **Auto Mode** and **Available** status.

| Property | Setting | Effect |
|---|---|---|
| Curiosity | On | Outer eye grows larger when looking sideways |
| Idle wander | On, 2s interval, ±3s variation | Eyes drift to random positions |
| Auto-blink | On, 3s interval, ±2s variation | Random natural blinking |

---

## Feature 6 — Physical Touch Sensor (GPIO 4)

Taps within a **400ms window** are counted together and trigger different actions depending on the current mode.

### Tap Behaviour
| Taps | In Eyes / Status / Auto | In Pomodoro mode | In Sleep |
|---|---|---|---|
| 1 | Cycle mode: Clock → Pomodoro Ready → Auto | Cycle mode | Wake → In Meeting |
| 2 | Laugh reaction (3s) + robot smile sound | Start focus session | — |
| 3+ | Confused shake (2s) → Angry (3s) | Reset to Pomodoro Ready | — |

> Tapping during an active notification dismisses it and restores the previous mode.

### Mode Cycle (1 tap)
`Clock → Pomodoro Ready → Auto → Clock → …`

---

## Feature 7 — Pet / Touch Simulation (Web UI)

The **Pat Head** button on the web UI sends tap counts to `/pet?taps=N`.

| Taps | Reaction | Sound | Duration |
|---|---|---|---|
| 1 | Cycles mode (same as physical 1-tap) | — | Instant |
| 2 | Happy + vertical laugh shake | Robot smile chirps | 3s |
| 3+ | Horizontal confused shake → Angry | Angry growl tones | 2s → 3s → restore |

### Web UI Feedback Text
| Taps | Text |
|---|---|
| 1 | `Happy!` |
| 2 | `Haha!!` |
| 3+ | `Hey!! Stop!!` |

> Previous mood and mode are fully restored after each pet interaction. Pomodoro keeps ticking in the background.

---

## Feature 8 — Clock Mode

Full OLED digital clock. Eye animation is paused.

| Element | Position | Format |
|---|---|---|
| Time | y=10, text size 2 | `HH:MM:SS` |
| Date | y=40, text size 1 | `DD-MM-YYYY` |

Press **Eyes** on the web UI or single-tap to cycle out of Clock Mode.

---

## Feature 9 — GitHub Notifications

Polls Firebase Realtime Database every **15 seconds** and shows a 3-phase notification overlay when a new GitHub event arrives.

### Pipeline
```
GitHub event → GitHub Actions → Firebase RTDB REST API → ESP32 polls → OLED notification
```

### Cloud Setup
| Component | Detail |
|---|---|
| Firebase project | `desk-buddy-007` |
| RTDB region | `asia-southeast1` |
| RTDB path | `/notifications/latest` |
| GitHub secret | `FIREBASE_DB_SECRET` |

### Tracked Events
| `type` value | GitHub trigger | OLED label |
|---|---|---|
| `commit` | Push to any branch | `New Commit!` |
| `pr_merged` | PR closed + merged | `PR MERGED!` |
| `pr_approved` | PR review approved | `PR APPROVED!` |
| `pr_comment` | Comment on a PR | `PR Comment` |

### 3-Phase Notification Display
| Phase | Duration | What shows | Eye reaction |
|---|---|---|---|
| 0 | 2s | Eye animation with mood | Happy + V-flicker (positive) / Tired + H-flicker (negative) |
| 1 | 3.5s | Full-screen text (label, message, author) | Paused |
| 2 | 2.5s | Eye animation again | Same as phase 0 |

> Positive events (`commit`, `pr_merged`, `pr_approved`): Happy mood + vertical flicker + positive chime.  
> Negative events (`pr_comment`): Tired mood + horizontal flicker + negative chime.

### Dismissing
Single tap on the physical touch sensor dismisses the notification at any phase and restores the previous mode.

### Boot Delay
Polling starts **15 seconds** after boot to allow WiFi and NTP to stabilise.

### Testing Without a Real Commit
```bash
curl -X PUT \
  "https://desk-buddy-007-default-rtdb.asia-southeast1.firebasedatabase.app/notifications/latest.json?auth=YOUR_DB_SECRET" \
  -H "Content-Type: application/json" \
  -d '{"type":"commit","message":"test: hello from curl","author":"you","timestamp":'"$(date +%s)"'}'
```
Valid `type` values: `commit`, `pr_merged`, `pr_approved`, `pr_comment`.

---

## Audio

All sounds are played through the MAX98357A via I2S on a dedicated FreeRTOS task (Core 0).

| Trigger | Sound | Description |
|---|---|---|
| Boot | Rising fanfare | C5 → E5 → G5 → C6 (with shimmer) |
| Positive notification | Positive chime | Ascending 3-note: E5 → A5 → C6 |
| Negative notification (`pr_comment`) | Negative chime | Descending 2-note: C5 → G4 |
| 2-tap pet (laugh) | Robot smile | FM-wobble chirps, R2-D2 style |
| 3+-tap pet (confused) | Angry growl | Low wobble tones + heavy bass |
| Pomodoro focus ends | Focus-end bell | Descending bell: C6 → A5 → G5 → E5 |
| Pomodoro break ends | Break-end alarm | 3× double beep → E6 sharp |

---

## Web UI Routes

| Route | Action |
|---|---|
| `GET /` | Serve the control page |
| `GET /status?v=available` | Set status: Available |
| `GET /status?v=busy` | Set status: Busy |
| `GET /status?v=meeting` | Set status: In Meeting |
| `GET /status?v=away` | Set status: Away |
| `GET /status?v=auto` | Return to Auto Mode |
| `GET /pomodoro?v=start` | Start focus session |
| `GET /pomodoro?v=reset` | Reset Pomodoro → Auto Mode |
| `GET /pet?taps=N` | Trigger pet reaction (N = 1, 2, 3+) |
| `GET /clock` | Switch to Clock Mode |
| `GET /eyes` | Switch to Auto Mode (eye animation) |
| `GET /sleep` | Force Sleep Mode |
| `GET /touch2?action=tap` | Web simulation of Touch 2 tap |
| `GET /touch2?action=longpress` | Web simulation of Touch 2 long-press (800ms) |

### Touch 2 (web button) Behaviour
| Action | From Clock | From Pomodoro | From anything else |
|---|---|---|---|
| Tap | → Pomodoro Focus (starts timer) | → Auto Mode | → Clock |
| Long-press (800ms) | — | → Auto Mode (reset) | — |

---

## RoboEyes Mood Reference

| Constant | Eyelid shape | Feeling |
|---|---|---|
| `DEFAULT` | Flat top | Neutral |
| `TIRED` | Drooping top (inner side) | Sleepy / calm |
| `ANGRY` | Drooping top (outer side) | Focused / intense |
| `HAPPY` | Raised bottom | Cheerful |

---

## Required Libraries

| Library | Purpose |
|---|---|
| `WiFi` | WiFi connection |
| `WebServer` | HTTP control interface |
| `HTTPClient` | Outbound HTTPS polling to Firebase |
| `WiFiClientSecure` | TLS for Firebase REST API |
| `ArduinoJson` | JSON parsing of RTDB response |
| `Adafruit_SSD1306` | OLED display driver |
| `FluxGarage_RoboEyes` | Animated eye expressions |
| `driver/i2s` | I2S audio output |

> **Include order matters:** `WiFiClientSecure` and `ArduinoJson` must be included **before** `FluxGarage_RoboEyes.h` to avoid macro conflicts (`N`, `E`, `S`, `W` compass defines clash with mbedTLS).

---

## Boot Sequence

1. OLED initialises
2. Eyes set to Tired mood, ambient personality enabled
3. WiFi connects — IP printed to Serial Monitor
4. NTP time sync (IST, UTC+5:30)
5. Web server starts on port 80
6. FreeRTOS tasks launched: `wifiTask`, `pollTask`, `audioTask` (all Core 0)
7. Speaker initialised (MAX98357A via I2S)
8. Boot jingle plays (C5 → E5 → G5 → C6 fanfare)
9. `anim_laugh()` plays on OLED
10. Auto Mode begins (Tired mood until first time-tick)
