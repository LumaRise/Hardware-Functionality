/*
  Serial Monitor commands:
    start   -> manually start sunrise
    stop    -> stop sunrise and turn LEDs off
    status  -> print current values
    0 / 1 / 2 -> change pattern
*/

#include <FastLED.h>

/* ================= LED SETUP ================= */
#define LED_PIN       5
#define NUM_LEDS      64
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB

CRGB leds[NUM_LEDS];

/* ================= CUSTOMIZABLE SETTINGS =================
  mainly edit this section only
===================================================== */

// Pattern choices:
// 0 = Full Glow
// 1 = Bottom-Up
// 2 = Center Expand
int patternId = 0;

// Sunrise color combo
CRGB startColor = CRGB(20, 0, 0);        // dark red
CRGB midColor   = CRGB(255, 100, 0);     // orange
CRGB endColor   = CRGB(255, 255, 220);   // warm white

// Brightness range
uint8_t minBrightness = 35;
uint8_t maxBrightness = 255;

// Optional safety cap for the whole matrix
uint8_t globalBrightnessCap = 50;

/* ==================================================== */

/* ================= ALARM SETTINGS ================= */
int sunriseOffsetSec   = 60;
int sunriseDurationSec = 60;

/* ================= SYSTEM STATE ================= */
enum AlarmState {
  IDLE,
  SUNRISE,
  ALARM
};

AlarmState currentState = IDLE;

bool sunriseRunning = false;
bool alarmTriggered = false;
unsigned long sunriseStartMs = 0;

/* ================= TIME DATA FROM ESP32 ================= */
int currentHour = 0;
int currentMinute = 0;
int currentSecond = 0;

int alarmHour = 7;
int alarmMinute = 0;
int alarmSecond = 0;

/* ================= STATUS TIMER ================= */
unsigned long lastStatusSendMs = 0;
const unsigned long STATUS_SEND_INTERVAL = 1000;

/* ================= XY HELPER =================
   Converts (x, y) to index for zig-zag wired 8x8 matrix
*/
int xy(int x, int y) {
  if (y % 2 == 0) return y * 8 + x;
  return y * 8 + (7 - x);
}

/* ================= TIME HELPERS ================= */

bool parseTime12(const String& input, int &h, int &m, int &s) {
  String t = input;
  t.trim();

  int sp = t.lastIndexOf(' ');
  if (sp < 0) return false;

  String timePart = t.substring(0, sp);
  String ampm = t.substring(sp + 1);
  ampm.toUpperCase();

  int c1 = timePart.indexOf(':');
  int c2 = timePart.indexOf(':', c1 + 1);

  if (c1 < 0 || c2 < 0) return false;

  int hh = timePart.substring(0, c1).toInt();
  int mm = timePart.substring(c1 + 1, c2).toInt();
  int ss = timePart.substring(c2 + 1).toInt();

  if (hh < 1 || hh > 12 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;

  if (ampm == "AM") {
    if (hh == 12) hh = 0;
  } else if (ampm == "PM") {
    if (hh != 12) hh += 12;
  } else {
    return false;
  }

  h = hh;
  m = mm;
  s = ss;
  return true;
}

long secondsSinceMidnight(int h, int m, int s) {
  return (long)h * 3600L + (long)m * 60L + s;
}

long secondsUntilEvent(long nowSec, long targetSec) {
  long diff = targetSec - nowSec;
  if (diff < 0) diff += 86400L;  // wrap to next day
  return diff;
}

String stateToString(AlarmState st) {
  if (st == IDLE) return "IDLE";
  if (st == SUNRISE) return "SUNRISE";
  if (st == ALARM) return "ALARM";
  return "IDLE";
}

/* ================= COLOR BLENDING =================
   Uses p (0.0 to 1.0) to blend:
   start -> mid -> end
*/
CRGB getSunriseColor(float p) {
  p = constrain(p, 0.0f, 1.0f);

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

/* ================= PATTERN 0: FULL GLOW ================= */
void showSunriseFullGlow(float p) {
  CRGB c = getSunriseColor(p);
  fill_solid(leds, NUM_LEDS, c);
  FastLED.show();
}

/* ================= PATTERN 1: BOTTOM-UP ================= */
void showSunriseBottomUp(float p) {
  CRGB c = getSunriseColor(p);
  FastLED.clear();

  int rowsLit = round(p * 8.0f);
  rowsLit = constrain(rowsLit, 0, 8);

  for (int y = 7; y >= 8 - rowsLit; y--) {
    for (int x = 0; x < 8; x++) {
      leds[xy(x, y)] = c;
    }
  }

  FastLED.show();
}

/* ================= PATTERN 2: CENTER EXPAND ================= */
void showSunriseCenterExpand(float p) {
  p = constrain(p, 0.0f, 1.0f);
  CRGB c = getSunriseColor(p);
  FastLED.clear();

  int stage = floor(p * 4.0f);
  stage = constrain(stage, 0, 4);

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

/* ================= PATTERN SWITCH ================= */
void showSunrise(float p) {
  if (patternId == 0) {
    showSunriseFullGlow(p);
  } else if (patternId == 1) {
    showSunriseBottomUp(p);
  } else if (patternId == 2) {
    showSunriseCenterExpand(p);
  } else {
    showSunriseFullGlow(p);
  }
}

/* ================= LED CONTROL ================= */
void clearLeds() {
  FastLED.clear();
  FastLED.show();
}

void showAlarmState() {
  fill_solid(leds, NUM_LEDS, endColor);
  FastLED.show();
}

/* ================= SERIAL FROM ESP32 ================= */
void handleSerial1FromESP32() {
  while (Serial1.available()) {
    String line = Serial1.readStringUntil('\n');
    line.trim();

    if (line.startsWith("TIME,")) {
      String value = line.substring(5);
      parseTime12(value, currentHour, currentMinute, currentSecond);
    }
    else if (line.startsWith("ALARM,")) {
      String value = line.substring(6);
      parseTime12(value, alarmHour, alarmMinute, alarmSecond);
    }
    else if (line.startsWith("SUNCFG,")) {
      int c1 = line.indexOf(',');
      int c2 = line.indexOf(',', c1 + 1);

      if (c1 > 0 && c2 > c1) {
        sunriseOffsetSec = line.substring(c1 + 1, c2).toInt();
        sunriseDurationSec = line.substring(c2 + 1).toInt();

        sunriseOffsetSec = constrain(sunriseOffsetSec, 0, 3600);
        sunriseDurationSec = constrain(sunriseDurationSec, 1, 7200);
      }
    }
  }
}

/* ================= SERIAL MONITOR COMMANDS ================= */
void printStatus() {
  Serial.println("----- STATUS -----");
  Serial.print("Current Time: ");
  Serial.print(currentHour); Serial.print(":");
  Serial.print(currentMinute); Serial.print(":");
  Serial.println(currentSecond);

  Serial.print("Alarm Time: ");
  Serial.print(alarmHour); Serial.print(":");
  Serial.print(alarmMinute); Serial.print(":");
  Serial.println(alarmSecond);

  Serial.print("Pattern: ");
  Serial.println(patternId);

  Serial.print("Offset Sec: ");
  Serial.println(sunriseOffsetSec);

  Serial.print("Duration Sec: ");
  Serial.println(sunriseDurationSec);

  Serial.print("State: ");
  Serial.println(stateToString(currentState));
  Serial.println("------------------");
}

void handleUsbSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "start") {
    sunriseRunning = true;
    alarmTriggered = false;
    sunriseStartMs = millis();
    currentState = SUNRISE;
    Serial.println("Manual sunrise started.");
  }
  else if (cmd == "stop") {
    sunriseRunning = false;
    alarmTriggered = false;
    currentState = IDLE;
    clearLeds();
    Serial.println("Stopped.");
  }
  else if (cmd == "status") {
    printStatus();
  }
  else if (cmd == "0" || cmd == "1" || cmd == "2") {
    patternId = cmd.toInt();
    Serial.print("Pattern set to: ");
    Serial.println(patternId);
  }
}

/* ================= STATE MACHINE ================= */
void updateAlarmLogic() {
  long nowSec = secondsSinceMidnight(currentHour, currentMinute, currentSecond);
  long alarmSec = secondsSinceMidnight(alarmHour, alarmMinute, alarmSecond);

  long sunriseStartSec = alarmSec - sunriseOffsetSec;
  while (sunriseStartSec < 0) sunriseStartSec += 86400L;

  long untilAlarm = secondsUntilEvent(nowSec, alarmSec);
  long untilSunrise = secondsUntilEvent(nowSec, sunriseStartSec);

  if (!sunriseRunning && !alarmTriggered) {
    if (untilSunrise == 0) {
      sunriseRunning = true;
      sunriseStartMs = millis();
      currentState = SUNRISE;
      Serial.println("Auto sunrise started.");
    }
  }

  if (sunriseRunning) {
    float elapsed = (millis() - sunriseStartMs) / 1000.0f;
    float p = elapsed / sunriseDurationSec;

    if (p >= 1.0f) {
      p = 1.0f;
      showSunrise(p);
    } else {
      showSunrise(p);
    }

    if (untilAlarm == 0) {
      sunriseRunning = false;
      alarmTriggered = true;
      currentState = ALARM;
      Serial.println("Alarm triggered.");
    }
  }

  if (alarmTriggered) {
    showAlarmState();
  }

  if (!sunriseRunning && !alarmTriggered) {
    currentState = IDLE;
  }
}

/* ================= STATUS BACK TO ESP32 ================= */
void sendStatusToESP32() {
  if (millis() - lastStatusSendMs < STATUS_SEND_INTERVAL) return;
  lastStatusSendMs = millis();

  long nowSec = secondsSinceMidnight(currentHour, currentMinute, currentSecond);
  long alarmSec = secondsSinceMidnight(alarmHour, alarmMinute, alarmSecond);
  long countdown = 0;

  if (currentState == SUNRISE || currentState == ALARM) {
    countdown = secondsUntilEvent(nowSec, alarmSec);
  }

  Serial1.print("STATUS,");
  Serial1.print(stateToString(currentState));
  Serial1.print(",");
  Serial1.println(countdown);
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(globalBrightnessCap);

  clearLeds();

  Serial.println("LumaRise Arduino ready.");
  Serial.println("Commands: start, stop, status, 0, 1, 2");
}

/* ================= LOOP ================= */
void loop() {
  handleSerial1FromESP32();
  handleUsbSerialCommands();
  updateAlarmLogic();
  sendStatusToESP32();
}
