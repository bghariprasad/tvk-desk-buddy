# TVK Desk Buddy — Feature Reference

## Hardware
| Component | Detail |
|---|---|
| Board | ESP32 |
| Display | SSD1306 OLED 128×64px, I2C (SDA=21, SCL=22, addr=0x3C) |
| Eyes library | FluxGarage RoboEyes v1.1.1 |
| Connectivity | WiFi (WebServer on port 80), HTTPS polling (Firebase RTDB) |
| Speaker amp | MAX98357A I2S mono amplifier |
| Speaker | 8Ω 0.5W |

### Pin Map
| GPIO | Role | Type |
|---|---|---|
| 4 | Touch sensor (pat-head tap) | Digital Input |
| 21 | I2C SDA — OLED display | I2C |
| 22 | I2C SCL — OLED display | I2C |
| 26 | MAX98357A BCLK — I2S bit clock | I2S (SPK) |
| 27 | MAX98357A LRC — I2S LR clock | I2S (SPK) |
| 33 | MAX98357A DIN — I2S data out | I2S (SPK) |

### MAX98357A Wiring
| MAX98357A Pin | ESP32 | Notes |
|---|---|---|
| VIN | 5V (VIN) | Use 5V — not 3.3V |
| GND | GND | — |
| BCLK | GPIO 26 | I2S bit clock |
| LRC | GPIO 27 | I2S left/right clock |
| DIN | GPIO 33 | I2S data |
| SD | 3.3V | Pull HIGH to enable amp |
| GAIN | (unconnected) | Floating = 9dB |
| OUT+ | Speaker + | — |
| OUT− | Speaker − | — |

> Software volume is set to **25%** (`SPK_VOLUME = 0.25f`) to protect the 0.5W speaker. Do not increase beyond 40% without checking speaker power limits.

### Speaker Config
| Parameter | Value |
|---|---|
| I2S port | I2S_NUM_1 |
| Sample rate | 44100 Hz |
| Bit depth | 16-bit |
| Channel | Stereo (MAX98357A sums to mono) |
| DMA buffers | 8 × 64 samples |
| Volume | 25% (`SPK_VOLUME = 0.25f`) |

---

## Feature 1 — Time-Aware Mood (Auto Mode)

The buddy automatically changes its eye expression based on the time of day. Active whenever **Auto Mode** is selected or the device boots.

| Time | Mood | Meaning |
|---|---|---|
| 06:00 – 08:59 | Tired | Early morning, just waking up |
| 09:00 – 11:59 | Default | Morning work hours |
| 12:00 – 12:59 | Happy | Lunch break |
| 13:00 – 17:59 | Default | Afternoon work hours |
| 18:00 – 20:59 | Happy | Evening wind-down |
| 21:00 – 05:59 | Tired | Night time |

> Time is synced via NTP (`pool.ntp.org`) on boot. Timezone: IST (UTC+5:30).

---

## Feature 2 — Pomodoro Timer

A focus/break cycle timer visible on both the OLED and the web UI.

### OLED Overlay (top strip, y=0–9px)
A small status strip is always drawn at the top of the screen during a Pomodoro session, leaving the eye animation in the lower area.

| Element | Position | Detail |
|---|---|---|
| Label + time | y=1, centered | `FOCUS 25:00` or `BREAK 05:00` |
| Progress bar | y=9, 1px tall | Fills left→right as session progresses |

### Eye behaviour during Pomodoro

| Phase | Eye mood | Eye position | Extra |
|---|---|---|---|
| Focus start | Angry | Center-bottom (S) | Infrequent blink (every 8s) |
| Break | Happy | Resets to center | Laugh animation plays |
| Overtime | Tired | — | Sweat drops animation |

### Web UI timer
The web UI shows a countdown with a colour-coded progress bar:

| Phase | Bar colour | Label |
|---|---|---|
| Focus | Blue | Stay focused |
| Break | Green | Break time! |
| Overtime | Red | Overtime - take a break! |

### Durations (currently set for testing)
| Phase | Duration |
|---|---|
| Focus | 25 seconds |
| Break | 5 seconds |

> Revert to `25UL * 60 * 1000` and `5UL * 60 * 1000` for production.

---

## Feature 3 — Status Indicator

Manually set your availability. Status is held until changed or Auto Mode is re-selected.

| Status | Eye mood | Ambient | Blink interval |
|---|---|---|---|
| Available | Happy | On (curiosity + idle wander) | 3s ±2s |
| Busy | Angry | Off | 6s (fixed) |
| In Meeting | Default | Curious + idle (fast) | 4s ±2s |
| Away | Tired | Off | 7s ±3s |
| Auto Mode | Time-based (see Feature 1) | On | 3s ±2s |

---

## Feature 4 — Ambient Personality

When active, the eyes feel alive — they wander, glance sideways, and blink naturally. Active during **Auto Mode** and **Available** status.

| Property | Setting | Effect |
|---|---|---|
| Curiosity | On | Outer eye grows larger when looking sideways |
| Idle wander | On, 2s interval, ±3s variation | Eyes drift to random positions |
| Auto-blink | On, 3s interval, ±2s variation | Random natural blinking |

---

## Feature 5 — Pet / Touch Simulation

Tap the **Pat Head** button on the web UI rapidly to trigger reactions. Taps within a 400ms window are counted together.

| Taps | Reaction | Duration | Then |
|---|---|---|---|
| 1 tap | Happy mood | 3 seconds | Restore previous mood |
| 2 taps | Happy mood + vertical laugh shake | 3 seconds | Restore previous mood |
| 3+ taps | Horizontal confused shake | 2 seconds | → Angry for 3s → Restore |

> The previous mood and mode (Auto / Status / Pomodoro) are fully restored after each pet interaction. Pomodoro keeps ticking in the background during a pet.

### Web UI feedback
| Taps | Feedback text |
|---|---|
| 1 | `Happy!` |
| 2 | `Haha!!` |
| 3+ | `Hey!! Stop!!` |

---

## Feature 6 — Clock Mode

Switches the full OLED display to a digital clock. Eye animation is paused.

| Element | Position | Format |
|---|---|---|
| Time | y=10, text size 2 | `HH:MM:SS` |
| Date | y=40, text size 1 | `DD-MM-YYYY` |

Press **Eyes** on the web UI to return to eye animation (Auto Mode).

---

## Web UI Routes

| Route | Method | Action |
|---|---|---|
| `/` | GET | Serve the control page |
| `/status?v=available` | GET | Set status: Available |
| `/status?v=busy` | GET | Set status: Busy |
| `/status?v=meeting` | GET | Set status: In Meeting |
| `/status?v=away` | GET | Set status: Away |
| `/status?v=auto` | GET | Return to Auto Mode |
| `/pomodoro?v=start` | GET | Start focus session |
| `/pomodoro?v=reset` | GET | Reset Pomodoro |
| `/pet?taps=N` | GET | Trigger pet reaction (N=1,2,3+) |
| `/clock` | GET | Switch to clock display |
| `/eyes` | GET | Switch back to eye animation |

---

## RoboEyes Mood Reference

| Constant | Eyelid shape | Feeling |
|---|---|---|
| `DEFAULT` | Flat top | Neutral |
| `TIRED` | Drooping top (inner side) | Sleepy / calm |
| `ANGRY` | Drooping top (outer side) | Focused / intense |
| `HAPPY` | Raised bottom | Cheerful |

---

## Feature 7 — GitHub Notifications

The buddy polls Firebase Realtime Database every 30 seconds and displays a notification overlay on the OLED whenever a GitHub event occurs.

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
| GitHub secret | `FIREBASE_DB_SECRET` — Firebase database secret |

### Tracked Events
| GitHub Event | Trigger condition | OLED line 1 | OLED line 2 |
|---|---|---|---|
| Push | Any branch | `GIT: New commit!` | Commit message (first line, 21 chars) |
| PR merged | `pull_request` closed + merged | `PR MERGED! :)` | PR title (21 chars) |
| PR approved | `pull_request_review` state = approved | `PR APPROVED! :D` | PR title (21 chars) |
| PR comment | `issue_comment` on a PR | `PR Comment!` | First line of comment (21 chars) |

### OLED Notification Behaviour
| Property | Detail |
|---|---|
| Display area | Bottom 14px strip (y=50–63), overlaid on eyes |
| Duration | 5 seconds, then restores previous mode |
| Dismiss early | Single tap on physical touch sensor |
| Eye reaction | HAPPY mood + vertical flicker for duration |
| Poll interval | 30 seconds |
| Boot delay | 15 seconds (waits for WiFi + NTP to stabilise) |

### Required Arduino Libraries
| Library | Purpose |
|---|---|
| `HTTPClient` | Outbound HTTPS polling |
| `WiFiClientSecure` | TLS for Firebase REST API |
| `ArduinoJson` | JSON parsing of RTDB response |

> **Include order matters:** `WiFiClientSecure` and `ArduinoJson` must be included **before** `FluxGarage_RoboEyes.h` to avoid macro conflicts (`N`, `E`, `S`, `W` compass defines clash with mbedTLS).

### Testing Without a Real Commit
Write directly to RTDB via curl to trigger a notification immediately:
```bash
curl -X PUT \
  "https://desk-buddy-007-default-rtdb.asia-southeast1.firebasedatabase.app/notifications/latest.json?auth=YOUR_DB_SECRET" \
  -H "Content-Type: application/json" \
  -d '{"type":"commit","message":"test: hello from curl","author":"you","repo":"TVK","timestamp":'"$(date +%s)"'}'
```
Valid `type` values: `commit`, `pr_merged`, `pr_approved`, `pr_comment`.

---

## Boot Sequence

1. OLED initialises — eyes open from closed (blink-in)
2. WiFi connects — IP printed to Serial Monitor
3. NTP time sync (IST)
4. Web server starts
5. `anim_laugh()` plays — boot greeting
6. Auto Mode begins with Tired mood (time-dependent after first tick)
