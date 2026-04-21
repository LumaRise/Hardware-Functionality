
/*
  LumaRise Arduino Uno R4 WiFi
  Companion sketch for the display/web controlle

  What this code does
  - Runs the sunrise lights using the same pattern logic as the test sketch
  - Plays looping alarm audio
  - Handles short-press snooze and long-press stop
  - Supports preview commands from the display board
  - Sends STATUS,<STATE>,<COUNTDOWN> back to the display board

  Alarm behavior
  - 10-minute timeout for each sounding cycle
  - 1 automatic snooze maximum
  - 5 manual snoozes maximum
  - Snoozed alarms restart sound only, without replaying the sunrise

  Serial messages expected from the display board
    TIME,hh:mm:ss AM/PM
    NEXT,id,hh:mm:ss AM/PM
    CFG,id,track,volume,effect,colorTheme,snoozeEnabled,snoozeDurSec,sunriseOffsetSec,sunriseDurSec
    PREVIEW,effect,colorTheme,durationSec
    PREVIEW_SOUND,track,volume,durationSec
    PREVIEW_STOP
*/

#include <FastLED.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <math.h>

/* ================= HARDWARE ================= */
#define LED_PIN          5
#define NUM_LEDS         64
#define LED_TYPE         WS2812B
#define COLOR_ORDER      GRB

#define BUTTON_PIN       2

#define DFPLAYER_RX_PIN  6
#define DFPLAYER_TX_PIN  7

CRGB leds[NUM_LEDS];
SoftwareSerial dfSerial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
DFRobotDFPlayerMini dfPlayer;

/* ================= SERIAL ================= */
#define SERIAL_BAUD_USB   115200
#define SERIAL_BAUD_LINK  9600
#define STATUS_INTERVAL   1000

/* ================= STATES ================= */
enum AlarmState {
  STATE_IDLE,
  STATE_SUNRISE,
  STATE_ALARM,
  STATE_SNOOZED,
  STATE_PREVIEW
};

AlarmState currentState = STATE_IDLE;

/* ================= CONFIG FROM ESP32 ================= */
struct AlarmConfig {
  int track = 1;
  int volume = 21;
  int effect = 0;
  int colorTheme = 0;
  bool snoozeEnabled = true;
  unsigned long snoozeDurSec = 300;
  unsigned long sunriseOffsetSec = 600;
  unsigned long sunriseDurSec = 600;
};

AlarmConfig cfg;

void clampAlarmConfig() {
  cfg.track = constrain(cfg.track, 1, 4);
  cfg.volume = constrain(cfg.volume, 0, 30);
  cfg.effect = constrain(cfg.effect, 0, 2);
  cfg.colorTheme = constrain(cfg.colorTheme, 0, 3);
  cfg.snoozeDurSec = max(60UL, cfg.snoozeDurSec);
  cfg.sunriseOffsetSec = max(60UL, cfg.sunriseOffsetSec);
  cfg.sunriseDurSec = max(60UL, cfg.sunriseDurSec);

  if (cfg.sunriseDurSec > cfg.sunriseOffsetSec) {
    cfg.sunriseDurSec = cfg.sunriseOffsetSec;
  }
}

/* ================= TIME / ALARM ================= */
bool timeValid = false;
unsigned long currentEpochSec = 0;

int nextAlarmId = -1;
bool nextAlarmValid = false;
unsigned long nextAlarmEpochSec = 0;

bool alarmArmed = false;
unsigned long armedAlarmEpochSec = 0;
unsigned long lastTriggeredAlarmEpochSec = 0;

/* ================= OPTION A ================= */
const unsigned long alarmTimeoutSec = 600;
const int MAX_MANUAL_SNOOZES = 5;
const int MAX_AUTO_SNOOZES = 1;

int manualSnoozeCount = 0;
int autoSnoozeCount = 0;

bool snoozeSkipsSunrise = false;
unsigned long alarmStartEpochSec = 0;

// If another saved alarm becomes relevant while snooze is active,
// it is staged here and can take over when its sunrise window begins.
bool pendingTakeoverValid = false;
bool pendingTakeoverCfgValid = false;
int pendingTakeoverId = -1;
unsigned long pendingTakeoverEpochSec = 0;
AlarmConfig pendingTakeoverCfg;

/* ================= PREVIEW ================= */
bool previewLightRunning = false;
bool previewSoundRunning = false;
unsigned long previewStartMs = 0;
unsigned long previewDurationMs = 0;

int previewEffect = 0;
int previewTheme = 0;
int previewTrack = 1;
int previewVolume = 21;

/* ================= BUTTON ================= */
bool lastButtonReading = HIGH;
bool buttonStableState = HIGH;
unsigned long lastDebounceMs = 0;
unsigned long buttonPressStartMs = 0;
const unsigned long debounceDelay = 30;
const unsigned long longPressMs = 1500;

/* ================= STATUS ================= */
unsigned long lastStatusMs = 0;

/* ================= SUNRISE STYLE ================= */
uint8_t minBrightness = 35;
uint8_t maxBrightness = 255;

/* ===================================================== */
/* ================= BASIC HELPERS ===================== */
/* ===================================================== */

String stateToString(AlarmState s) {
  switch (s) {
    case STATE_IDLE:    return "IDLE";
    case STATE_SUNRISE: return "SUNRISE";
    case STATE_ALARM:   return "ALARM";
    case STATE_SNOOZED: return "SNOOZED";
    case STATE_PREVIEW: return "PREVIEW";
    default:            return "IDLE";
  }
}

void stopAudio() {
  dfPlayer.stop();
}

void setVolumeClamped(int vol) {
  vol = constrain(vol, 0, 30);
  dfPlayer.volume(vol);
}

void playTrackLooping(int track) {
  track = constrain(track, 1, 4);
  dfPlayer.loop(track);
}

void clearLeds() {
  FastLED.clear();
  FastLED.show();
}

void resetAlarmCycleCounters() {
  manualSnoozeCount = 0;
  autoSnoozeCount = 0;
  snoozeSkipsSunrise = false;
  alarmStartEpochSec = 0;
}

int parse12HourToSecondsOfDay(const String &timeStr) {
  String s = timeStr;
  s.trim();

  int c1 = s.indexOf(':');
  int c2 = s.indexOf(':', c1 + 1);
  int sp = s.lastIndexOf(' ');

  if (c1 < 0 || c2 < 0 || sp < 0) return -1;

  int hh = s.substring(0, c1).toInt();
  int mm = s.substring(c1 + 1, c2).toInt();
  int ss = s.substring(c2 + 1, sp).toInt();
  String ampm = s.substring(sp + 1);
  ampm.trim();
  ampm.toUpperCase();

  if (hh < 1 || hh > 12 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return -1;

  int h24 = hh % 12;
  if (ampm == "PM") h24 += 12;

  return h24 * 3600 + mm * 60 + ss;
}

void updateRunningClock() {
  if (!timeValid) return;

  static unsigned long lastMs = millis();
  unsigned long nowMs = millis();
  unsigned long deltaMs = nowMs - lastMs;
  lastMs = nowMs;

  if (deltaMs >= 1000) {
    currentEpochSec += deltaMs / 1000UL;
  }
}

unsigned long secondsUntil(unsigned long targetEpoch) {
  if (!timeValid || targetEpoch <= currentEpochSec) return 0;
  return targetEpoch - currentEpochSec;
}

/* ===================================================== */
/* ========== SUNRISE TEST CODE MATCH SECTION ========== */
/* ===================================================== */

int xy(int x, int y) {
  if (y % 2 == 0) return y * 8 + x;
  else            return y * 8 + (7 - x);
}

void getThemeColors(int theme, CRGB &startColor, CRGB &midColor, CRGB &endColor) {
  switch (theme) {
    case 1:
      startColor = CRGB(40, 10, 0);
      midColor   = CRGB(255, 140, 20);
      endColor   = CRGB(255, 240, 180);
      break;
    case 2:
      startColor = CRGB(60, 0, 20);
      midColor   = CRGB(255, 80, 0);
      endColor   = CRGB(255, 220, 160);
      break;
    case 3:
      startColor = CRGB(30, 0, 10);
      midColor   = CRGB(255, 80, 150);
      endColor   = CRGB(255, 220, 240);
      break;
    case 0:
    default:
      startColor = CRGB(20, 0, 0);
      midColor   = CRGB(255, 100, 0);
      endColor   = CRGB(255, 255, 220);
      break;
  }
}

CRGB getSunriseColor(float p, int theme) {
  p = constrain(p, 0.0f, 1.0f);

  CRGB startColor, midColor, endColor;
  getThemeColors(theme, startColor, midColor, endColor);

  CRGB c;

  if (p < 0.5f) {
    float t = p / 0.5f;
    c.r = startColor.r + (midColor.r - startColor.r) * t;
    c.g = startColor.g + (midColor.g - startColor.g) * t;
    c.b = startColor.b + (midColor.b - startColor.b) * t;
  } else {
    float t = (p - 0.5f) / 0.5f;
    c.r = midColor.r + (endColor.r - midColor.r) * t;
    c.g = midColor.g + (endColor.g - midColor.g) * t;
    c.b = midColor.b + (endColor.b - midColor.b) * t;
  }

  uint8_t scaledBrightness = map((int)(p * 100.0f), 0, 100, minBrightness, maxBrightness);
  c.nscale8_video(scaledBrightness);

  return c;
}

void showSunriseFullGlow(float p, int theme) {
  CRGB c = getSunriseColor(p, theme);
  fill_solid(leds, NUM_LEDS, c);
  FastLED.show();
}

void showSunriseBottomUp(float p, int theme) {
  CRGB c = getSunriseColor(p, theme);
  FastLED.clear();

  int rowsLit = round(p * 8.0f);

  for (int y = 7; y >= 8 - rowsLit; y--) {
    for (int x = 0; x < 8; x++) {
      leds[xy(x, y)] = c;
    }
  }

  FastLED.show();
}

void showSunriseCenterExpand(float p, int theme) {
  p = constrain(p, 0.0f, 1.0f);
  CRGB c = getSunriseColor(p, theme);

  FastLED.clear();

  int stage = floor(p * 4.0f);

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int dx = min(abs(x - 3), abs(x - 4));
      int dy = min(abs(y - 3), abs(y - 4));
      int layer = max(dx, dy);

      if (layer <= stage) {
        leds[xy(x, y)] = c;
      }
    }
  }

  FastLED.show();
}

void showSunrise(float p, int effect, int theme) {
  if (effect == 0) {
    showSunriseFullGlow(p, theme);
  } else if (effect == 1) {
    showSunriseBottomUp(p, theme);
  } else if (effect == 2) {
    showSunriseCenterExpand(p, theme);
  } else {
    showSunriseFullGlow(p, theme);
  }
}

/* ===================================================== */
/* ================= SERIAL PARSING ==================== */
/* ===================================================== */

void handleTimeLine(const String &line) {
  String payload = line.substring(5);
  int secOfDay = parse12HourToSecondsOfDay(payload);
  if (secOfDay < 0) return;

  if (!timeValid) {
    currentEpochSec = (unsigned long)secOfDay;
    timeValid = true;
    Serial.println("Time sync valid.");
  } else {
    unsigned long oldSecOfDay = currentEpochSec % 86400UL;
    if ((int)oldSecOfDay > 80000 && secOfDay < 4000) {
      currentEpochSec += (86400UL - oldSecOfDay);
      currentEpochSec += (unsigned long)secOfDay;
    } else {
      currentEpochSec = (currentEpochSec / 86400UL) * 86400UL + (unsigned long)secOfDay;
    }
  }
}

void handleNextLine(const String &line) {
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return;

  int incomingId = line.substring(c1 + 1, c2).toInt();
  String timePart = line.substring(c2 + 1);
  timePart.trim();

  // Keep an active snooze local to the Arduino. A different upcoming alarm
  // is staged and can take over later when its sunrise window begins.
  if (currentState == STATE_SNOOZED) {
    if (incomingId < 0) {
      Serial.println("Ignoring NEXT=-1 during SNOOZED state.");
      return;
    }

    if (incomingId == nextAlarmId) {
      Serial.println("Ignoring NEXT update for current snoozed alarm.");
      return;
    }

    int targetSecOfDay = parse12HourToSecondsOfDay(timePart);
    if (targetSecOfDay < 0) return;

    unsigned long todayBase = (currentEpochSec / 86400UL) * 86400UL;
    unsigned long candidate = todayBase + (unsigned long)targetSecOfDay;
    if (candidate <= currentEpochSec) candidate += 86400UL;

    pendingTakeoverValid = true;
    pendingTakeoverCfgValid = false;
    pendingTakeoverId = incomingId;
    pendingTakeoverEpochSec = candidate;

    Serial.print("Staged takeover alarm id=");
    Serial.print(pendingTakeoverId);
    Serial.print(" epoch=");
    Serial.println(pendingTakeoverEpochSec);
    return;
  }

  nextAlarmId = incomingId;

  if (incomingId < 0 || !timeValid) {
    nextAlarmValid = false;
    nextAlarmEpochSec = 0;
    alarmArmed = false;
    return;
  }

  int targetSecOfDay = parse12HourToSecondsOfDay(timePart);
  if (targetSecOfDay < 0) {
    nextAlarmValid = false;
    nextAlarmEpochSec = 0;
    alarmArmed = false;
    return;
  }

  unsigned long todayBase = (currentEpochSec / 86400UL) * 86400UL;
  unsigned long candidate = todayBase + (unsigned long)targetSecOfDay;

  if (candidate <= currentEpochSec) candidate += 86400UL;

  nextAlarmEpochSec = candidate;
  nextAlarmValid = true;

  if (nextAlarmEpochSec != armedAlarmEpochSec) {
    armedAlarmEpochSec = nextAlarmEpochSec;
    alarmArmed = true;
    resetAlarmCycleCounters();

    Serial.print("Armed new alarm for epoch: ");
    Serial.println(armedAlarmEpochSec);
  }
}

void handleCfgLine(const String &line) {
  int parts[10];
  int count = 0;

  for (int i = 0; i < line.length() && count < 10; i++) {
    if (line.charAt(i) == ',') parts[count++] = i;
  }

  if (count < 9) return;

  int parsedId = line.substring(parts[0] + 1, parts[1]).toInt();

  AlarmConfig parsedCfg;
  parsedCfg.track            = constrain(line.substring(parts[1] + 1, parts[2]).toInt(), 1, 4);
  parsedCfg.volume           = constrain(line.substring(parts[2] + 1, parts[3]).toInt(), 0, 30);
  parsedCfg.effect           = constrain(line.substring(parts[3] + 1, parts[4]).toInt(), 0, 2);
  parsedCfg.colorTheme       = constrain(line.substring(parts[4] + 1, parts[5]).toInt(), 0, 3);
  parsedCfg.snoozeEnabled    = line.substring(parts[5] + 1, parts[6]).toInt() == 1;
  parsedCfg.snoozeDurSec     = max(1UL, (unsigned long)line.substring(parts[6] + 1, parts[7]).toInt());
  parsedCfg.sunriseOffsetSec = max(1UL, (unsigned long)line.substring(parts[7] + 1, parts[8]).toInt());
  parsedCfg.sunriseDurSec    = max(1UL, (unsigned long)line.substring(parts[8] + 1).toInt());

  AlarmConfig oldCfg = cfg;
  cfg = parsedCfg;
  clampAlarmConfig();
  parsedCfg = cfg;
  cfg = oldCfg;

  if (currentState == STATE_SNOOZED && pendingTakeoverValid && parsedId == pendingTakeoverId) {
    pendingTakeoverCfg = parsedCfg;
    pendingTakeoverCfgValid = true;

    Serial.print("Takeover CFG staged -> id=");
    Serial.print(parsedId);
    Serial.print(" sunriseOffset=");
    Serial.print(pendingTakeoverCfg.sunriseOffsetSec);
    Serial.print(" sunriseDur=");
    Serial.println(pendingTakeoverCfg.sunriseDurSec);
    return;
  }

  cfg = parsedCfg;

  Serial.print("CFG parsed -> id=");
  Serial.print(parsedId);
  Serial.print(" track=");
  Serial.print(cfg.track);
  Serial.print(" vol=");
  Serial.print(cfg.volume);
  Serial.print(" fx=");
  Serial.print(cfg.effect);
  Serial.print(" theme=");
  Serial.print(cfg.colorTheme);
  Serial.print(" snoozeEn=");
  Serial.print(cfg.snoozeEnabled ? 1 : 0);
  Serial.print(" snoozeSec=");
  Serial.print(cfg.snoozeDurSec);
  Serial.print(" sunriseOffset=");
  Serial.print(cfg.sunriseOffsetSec);
  Serial.print(" sunriseDur=");
  Serial.println(cfg.sunriseDurSec);
}

void startPreviewLight(int effect, int theme, int durationSec) {
  stopAudio();
  previewSoundRunning = false;
  previewLightRunning = true;
  previewStartMs = millis();
  previewDurationMs = (unsigned long)durationSec * 1000UL;
  previewEffect = effect;
  previewTheme = theme;
  currentState = STATE_PREVIEW;
  Serial.println("Preview light started.");
}

void startPreviewSound(int track, int volume, int durationSec) {
  clearLeds();
  stopAudio();

  previewLightRunning = false;
  previewSoundRunning = true;
  previewStartMs = millis();
  previewDurationMs = (unsigned long)durationSec * 1000UL;
  previewTrack = constrain(track, 1, 4);
  previewVolume = constrain(volume, 0, 30);

  setVolumeClamped(previewVolume);
  playTrackLooping(previewTrack);
  currentState = STATE_PREVIEW;
  Serial.println("Preview sound started.");
}

void stopPreview() {
  previewLightRunning = false;
  previewSoundRunning = false;
  stopAudio();
  clearLeds();

  if (currentState == STATE_PREVIEW) currentState = STATE_IDLE;
  Serial.println("Preview stopped.");
}

void handlePreviewLine(const String &line) {
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  int c3 = line.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return;

  int effect = constrain(line.substring(c1 + 1, c2).toInt(), 0, 2);
  int theme  = constrain(line.substring(c2 + 1, c3).toInt(), 0, 3);
  int dur    = max(1, line.substring(c3 + 1).toInt());

  startPreviewLight(effect, theme, dur);
}

void handlePreviewSoundLine(const String &line) {
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  int c3 = line.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return;

  int track = constrain(line.substring(c1 + 1, c2).toInt(), 1, 4);
  int vol   = constrain(line.substring(c2 + 1, c3).toInt(), 0, 30);
  int dur   = max(1, line.substring(c3 + 1).toInt());

  startPreviewSound(track, vol, dur);
}

void handleIncomingLine(String line) {
  line.trim();
  if (!line.length()) return;

  if (line.startsWith("TIME,")) {
    handleTimeLine(line);
  } else if (line.startsWith("NEXT,")) {
    handleNextLine(line);
  } else if (line.startsWith("CFG,")) {
    handleCfgLine(line);
  } else if (line.startsWith("PREVIEW_SOUND,")) {
    handlePreviewSoundLine(line);
  } else if (line.startsWith("PREVIEW,")) {
    handlePreviewLine(line);
  } else if (line.startsWith("PREVIEW_STOP")) {
    stopPreview();
  }
}

void readEsp32Serial() {
  while (Serial1.available()) {
    String line = Serial1.readStringUntil('\n');
    handleIncomingLine(line);
  }
}

/* ===================================================== */
/* ================= BUTTON CONTROL ==================== */
/* ===================================================== */

void fullStopAlarm() {
  stopAudio();
  clearLeds();

  currentState = STATE_IDLE;
  alarmStartEpochSec = 0;
  snoozeSkipsSunrise = false;

  // Clear the current alarm cycle so it cannot restart immediately.
  alarmArmed = false;
  nextAlarmValid = false;
  nextAlarmId = -1;
  nextAlarmEpochSec = 0;
  lastTriggeredAlarmEpochSec = 0;

  pendingTakeoverValid = false;
  pendingTakeoverCfgValid = false;
  pendingTakeoverId = -1;
  pendingTakeoverEpochSec = 0;

  Serial.println("Alarm fully stopped.");
}

// Short press uses this path.
void manualSnoozeAlarm() {
  if (!cfg.snoozeEnabled || !timeValid) {
    fullStopAlarm();
    return;
  }

  if (manualSnoozeCount >= MAX_MANUAL_SNOOZES) {
    Serial.println("Manual snooze limit reached. Stopping alarm.");
    fullStopAlarm();
    return;
  }

  manualSnoozeCount++;

  stopAudio();
  clearLeds();

  nextAlarmEpochSec = currentEpochSec + cfg.snoozeDurSec;
  nextAlarmValid = true;
  currentState = STATE_SNOOZED;
  snoozeSkipsSunrise = true;
  alarmStartEpochSec = 0;

  pendingTakeoverValid = false;
  pendingTakeoverCfgValid = false;
  pendingTakeoverId = -1;
  pendingTakeoverEpochSec = 0;

  Serial.print("Manual snooze ");
  Serial.print(manualSnoozeCount);
  Serial.print("/");
  Serial.println(MAX_MANUAL_SNOOZES);
}

// This runs only when the alarm times out on its own.
void autoSnoozeAlarm() {
  if (!cfg.snoozeEnabled || !timeValid) {
    fullStopAlarm();
    return;
  }

  if (autoSnoozeCount >= MAX_AUTO_SNOOZES) {
    Serial.println("Auto snooze limit reached. Stopping alarm.");
    fullStopAlarm();
    return;
  }

  autoSnoozeCount++;

  stopAudio();
  clearLeds();

  nextAlarmEpochSec = currentEpochSec + cfg.snoozeDurSec;
  nextAlarmValid = true;
  currentState = STATE_SNOOZED;
  snoozeSkipsSunrise = true;
  alarmStartEpochSec = 0;

  pendingTakeoverValid = false;
  pendingTakeoverCfgValid = false;
  pendingTakeoverId = -1;
  pendingTakeoverEpochSec = 0;

  Serial.print("Auto snooze ");
  Serial.print(autoSnoozeCount);
  Serial.print("/");
  Serial.println(MAX_AUTO_SNOOZES);
}

// Button behavior:
 // short press during sunrise/alarm = snooze
 // long press during sunrise/alarm = stop
void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > debounceDelay) {
    if (reading != buttonStableState) {
      buttonStableState = reading;

      if (buttonStableState == LOW) {
        buttonPressStartMs = millis();
      } else {
        unsigned long pressDur = millis() - buttonPressStartMs;

        if (currentState == STATE_ALARM) {
          Serial.print("Button released during ALARM. pressDur=");
          Serial.println(pressDur);

          if (pressDur >= longPressMs) fullStopAlarm();
          else manualSnoozeAlarm();
        }
        else if (currentState == STATE_SUNRISE) {
          Serial.print("Button released during SUNRISE. pressDur=");
          Serial.println(pressDur);

          if (pressDur >= longPressMs) fullStopAlarm();
          else manualSnoozeAlarm();
        }
        else if (currentState == STATE_PREVIEW) {
          stopPreview();
        }
      }
    }
  }

  lastButtonReading = reading;
}

/* ===================================================== */
/* ================== MAIN LOGIC ======================= */
/* ===================================================== */

void runPreview() {
  if (previewLightRunning) {
    float elapsed = (millis() - previewStartMs) / 1000.0f;
    float p = elapsed / (previewDurationMs / 1000.0f);
    if (p >= 1.0f) p = 1.0f;
    showSunrise(p, previewEffect, previewTheme);
  }

  if ((previewLightRunning || previewSoundRunning) &&
      (millis() - previewStartMs >= previewDurationMs)) {
    stopPreview();
  }
}

void runAlarmStateMachine() {
  if (currentState == STATE_PREVIEW) {
    runPreview();
    return;
  }

  if (!timeValid || !nextAlarmValid || nextAlarmId < 0 || !alarmArmed) {
    if (currentState != STATE_IDLE) {
      stopAudio();
      clearLeds();
      currentState = STATE_IDLE;
      alarmStartEpochSec = 0;
    }
    return;
  }

  unsigned long toAlarm = secondsUntil(nextAlarmEpochSec);
  unsigned long sunriseStartEpoch =
    (nextAlarmEpochSec > cfg.sunriseOffsetSec) ? nextAlarmEpochSec - cfg.sunriseOffsetSec : 0;

  static unsigned long lastDbgMs = 0;
  if (millis() - lastDbgMs > 3000) {
    lastDbgMs = millis();
    Serial.print("DBG now=");
    Serial.print(currentEpochSec);
    Serial.print(" next=");
    Serial.print(nextAlarmEpochSec);
    Serial.print(" armed=");
    Serial.print(alarmArmed ? 1 : 0);
    Serial.print(" until=");
    Serial.print(toAlarm);
    Serial.print(" sunriseStart=");
    Serial.print(sunriseStartEpoch);
    Serial.print(" state=");
    Serial.print(stateToString(currentState));
    Serial.print(" manualSnooze=");
    Serial.print(manualSnoozeCount);
    Serial.print("/");
    Serial.print(MAX_MANUAL_SNOOZES);
    Serial.print(" autoSnooze=");
    Serial.print(autoSnoozeCount);
    Serial.print("/");
    Serial.print(MAX_AUTO_SNOOZES);
    Serial.print(" skipSunrise=");
    Serial.println(snoozeSkipsSunrise ? 1 : 0);
  }

  if (currentEpochSec >= nextAlarmEpochSec) {
    if (lastTriggeredAlarmEpochSec != nextAlarmEpochSec || currentState != STATE_ALARM) {
      lastTriggeredAlarmEpochSec = nextAlarmEpochSec;
      currentState = STATE_ALARM;
      alarmStartEpochSec = currentEpochSec;

      showSunrise(1.0f, cfg.effect, cfg.colorTheme);
      setVolumeClamped(cfg.volume);
      playTrackLooping(cfg.track);

      Serial.println("Alarm triggered. Sound looping.");
    } else {
      showSunrise(1.0f, cfg.effect, cfg.colorTheme);
    }

    unsigned long alarmElapsed = currentEpochSec - alarmStartEpochSec;
    if (alarmElapsed >= alarmTimeoutSec) {
      Serial.println("Alarm timed out.");

      if (autoSnoozeCount < MAX_AUTO_SNOOZES) {
        autoSnoozeAlarm();
      } else {
        fullStopAlarm();
      }
    }
    return;
  }

  if (currentState == STATE_SNOOZED &&
      pendingTakeoverValid &&
      pendingTakeoverCfgValid) {
    unsigned long takeoverSunriseStart =
      (pendingTakeoverEpochSec > pendingTakeoverCfg.sunriseOffsetSec)
      ? pendingTakeoverEpochSec - pendingTakeoverCfg.sunriseOffsetSec
      : 0;

    if (currentEpochSec >= takeoverSunriseStart) {
      Serial.print("Pending takeover starting for alarm id=");
      Serial.println(pendingTakeoverId);

      nextAlarmId = pendingTakeoverId;
      nextAlarmEpochSec = pendingTakeoverEpochSec;
      cfg = pendingTakeoverCfg;
      nextAlarmValid = true;
      alarmArmed = true;
      armedAlarmEpochSec = nextAlarmEpochSec;
      snoozeSkipsSunrise = false;
      currentState = STATE_IDLE;
      resetAlarmCycleCounters();

      pendingTakeoverValid = false;
      pendingTakeoverCfgValid = false;
      pendingTakeoverId = -1;
      pendingTakeoverEpochSec = 0;
    }
  }

  if (!snoozeSkipsSunrise &&
      currentEpochSec >= sunriseStartEpoch &&
      currentEpochSec < nextAlarmEpochSec) {
    currentState = STATE_SUNRISE;

    unsigned long sunriseElapsed = currentEpochSec - sunriseStartEpoch;
    float p = (cfg.sunriseDurSec == 0)
      ? 1.0f
      : (float)sunriseElapsed / (float)cfg.sunriseDurSec;

    p = constrain(p, 0.0f, 1.0f);
    showSunrise(p, cfg.effect, cfg.colorTheme);
    return;
  }

  stopAudio();
  clearLeds();

  if (currentState == STATE_SNOOZED) {
    // remain snoozed until nextAlarmEpochSec
  } else {
    currentState = STATE_IDLE;
  }
}

unsigned long currentCountdownSec() {
  if (nextAlarmValid) return secondsUntil(nextAlarmEpochSec);
  return 0;
}

void sendStatus() {
  if (millis() - lastStatusMs < STATUS_INTERVAL) return;
  lastStatusMs = millis();

  String msg = "STATUS," + stateToString(currentState) + "," + String(currentCountdownSec());
  Serial1.println(msg);
  Serial.println(msg);
}

/* ===================================================== */
/* ============== OPTIONAL USB TEST COMMANDS =========== */
/* ===================================================== */

void handleUsbCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "stop") {
    fullStopAlarm();
    nextAlarmValid = false;
    nextAlarmId = -1;
    alarmArmed = false;
    Serial.println("Stopped and cleared alarm.");
  }
  else if (cmd == "snooze") {
    manualSnoozeAlarm();
  }
  else if (cmd == "testlight") {
    startPreviewLight(cfg.effect, cfg.colorTheme, 15);
  }
  else if (cmd == "testsound") {
    startPreviewSound(cfg.track, cfg.volume, 10);
  }
  else if (cmd == "previewstop") {
    stopPreview();
  }
  else if (cmd == "status") {
    Serial.print("state=");
    Serial.print(stateToString(currentState));
    Serial.print(" nextAlarmValid=");
    Serial.print(nextAlarmValid);
    Serial.print(" countdown=");
    Serial.print(currentCountdownSec());
    Serial.print(" manualSnooze=");
    Serial.print(manualSnoozeCount);
    Serial.print("/");
    Serial.print(MAX_MANUAL_SNOOZES);
    Serial.print(" autoSnooze=");
    Serial.print(autoSnoozeCount);
    Serial.print("/");
    Serial.println(MAX_AUTO_SNOOZES);
  }
}

/* ===================================================== */
/* ================= SETUP / LOOP ====================== */
/* ===================================================== */

void setup() {
  Serial.begin(SERIAL_BAUD_USB);
  Serial1.begin(SERIAL_BAUD_LINK);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(50);
  FastLED.clear();
  FastLED.show();

  dfSerial.begin(9600);
  delay(500);

  if (dfPlayer.begin(dfSerial)) {
    dfPlayer.volume(20);
    Serial.println("DFPlayer ready.");
  } else {
    Serial.println("DFPlayer not detected.");
  }

  Serial.println("LumaRise Arduino ready.");
  Serial.println("Snooze Settings: 10-minute timeout, 1 auto snooze, 5 manual snoozes.");
}

void loop() {
  updateRunningClock();
  readEsp32Serial();
  handleUsbCommands();
  handleButton();
  runAlarmStateMachine();
  sendStatus();
}
