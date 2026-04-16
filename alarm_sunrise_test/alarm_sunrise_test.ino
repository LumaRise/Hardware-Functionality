#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <FastLED.h>

/* =====================================================
   LumaRise Student Pattern + Sound Test
   -----------------------------------------------------
   Students can:
   - choose a pattern
   - choose a color combo
   - type "play" to test sunrise + sound
   - use button: short press = snooze, long press = stop

   SERIAL COMMANDS:
   play           -> start sunrise
   stop           -> stop everything
   0 / 1 / 2      -> choose pattern
   c0 / c1 / c2 / c3 / c4 -> choose color combo
   help           -> show commands
   ===================================================== */

/* ================= LED SETUP ================= */
#define LED_PIN       5
#define NUM_LEDS      64
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB
#define BRIGHTNESS    50

CRGB leds[NUM_LEDS];

/* ================= DFPLAYER SETUP ================= */
SoftwareSerial mySoftwareSerial(6, 7); // RX, TX
DFRobotDFPlayerMini myDFPlayer;

/* ================= BUTTON SETUP ================= */
#define BUTTON_PIN 2

/* ================= TIMING ================= */
const unsigned long sunriseDuration = 20000; // 20 sec for testing
const unsigned long longPressMs     = 900;
const unsigned long debounceMs      = 40;
const unsigned long snoozeMs        = 10000; // 10 sec test snooze

/* ================= STUDENT SETTINGS ================= */
// Pattern choices:
// 0 = Full Glow
// 1 = Bottom Up
// 2 = Center Expand
int patternId = 0;

// Color combo choices:
// 0 = Classic Sunrise
// 1 = Soft Golden Morning
// 2 = Deep Sunset to Sunrise
// 3 = Pink Sunrise
// 4 = Peach Sky
int colorComboId = 0;

// Brightness scaling inside the sunrise effect
uint8_t minBrightness = 35;
uint8_t maxBrightness = 255;

/* ================= STATE ================= */
bool isPlaying = false;
bool sunriseComplete = false;
bool isSnoozing = false;

unsigned long sunriseStartTime = 0;
unsigned long snoozeStartTime = 0;

/* ================= BUTTON TRACKING ================= */
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;

bool pressInProgress = false;
unsigned long pressStartTime = 0;

/* ================= COLOR VARIABLES ================= */
CRGB startColor;
CRGB midColor;
CRGB endColor;

/* =====================================================
   XY HELPER
   Converts (x, y) to LED index for zig-zag wiring
   ===================================================== */
int xy(int x, int y) {
  if (y % 2 == 0) return y * 8 + x;
  else            return y * 8 + (7 - x);
}

/* =====================================================
   COLOR COMBOS
   ===================================================== */
void setColorCombo(int comboId) {
  colorComboId = comboId;

  switch (colorComboId) {
    case 0: // Classic Sunrise
      startColor = CRGB(20, 0, 0);
      midColor   = CRGB(255, 100, 0);
      endColor   = CRGB(255, 255, 220);
      break;

    case 1: // Soft Golden Morning
      startColor = CRGB(40, 10, 0);
      midColor   = CRGB(255, 140, 20);
      endColor   = CRGB(255, 240, 180);
      break;

    case 2: // Deep Sunset to Sunrise
      startColor = CRGB(60, 0, 20);
      midColor   = CRGB(255, 80, 0);
      endColor   = CRGB(255, 220, 170);
      break;

    case 3: // Pink Sunrise
      startColor = CRGB(35, 0, 15);
      midColor   = CRGB(255, 90, 120);
      endColor   = CRGB(255, 220, 210);
      break;

    case 4: // Peach Sky
      startColor = CRGB(25, 5, 0);
      midColor   = CRGB(255, 120, 60);
      endColor   = CRGB(255, 230, 190);
      break;

    default:
      startColor = CRGB(20, 0, 0);
      midColor   = CRGB(255, 100, 0);
      endColor   = CRGB(255, 255, 220);
      colorComboId = 0;
      break;
  }
}

/* =====================================================
   SUNRISE COLOR BLENDING
   p = 0.0 -> start
   p = 1.0 -> end
   ===================================================== */
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

  uint8_t scaledBrightness = map((int)(p * 100), 0, 100, minBrightness, maxBrightness);
  c.nscale8_video(scaledBrightness);

  return c;
}

/* =====================================================
   PATTERNS
   ===================================================== */
void showSunriseFullGlow(float p) {
  CRGB c = getSunriseColor(p);
  fill_solid(leds, NUM_LEDS, c);
  FastLED.show();
}

void showSunriseBottomUp(float p) {
  CRGB c = getSunriseColor(p);
  FastLED.clear();

  int rowsLit = round(p * 8.0f);

  for (int y = 7; y >= 8 - rowsLit; y--) {
    for (int x = 0; x < 8; x++) {
      leds[xy(x, y)] = c;
    }
  }

  FastLED.show();
}

void showSunriseCenterExpand(float p) {
  p = constrain(p, 0.0f, 1.0f);
  CRGB c = getSunriseColor(p);

  FastLED.clear();

  int stage = floor(p * 4.0f);
  if (stage > 3) stage = 3;

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

void showSunrise(float p) {
  if (patternId == 0) {
    showSunriseFullGlow(p);
  }
  else if (patternId == 1) {
    showSunriseBottomUp(p);
  }
  else if (patternId == 2) {
    showSunriseCenterExpand(p);
  }
  else {
    showSunriseFullGlow(p);
  }
}

/* =====================================================
   SYSTEM HELPERS
   ===================================================== */
void stopEverything() {
  isPlaying = false;
  sunriseComplete = false;
  isSnoozing = false;

  myDFPlayer.stop();
  FastLED.clear();
  FastLED.show();

  Serial.println("STOP: LEDs + sound OFF.");
}

void startSunrise() {
  Serial.println("Starting sunrise simulation...");
  isPlaying = true;
  sunriseComplete = false;
  isSnoozing = false;
  sunriseStartTime = millis();
}

void startSnooze() {
  if (!isPlaying) return;

  Serial.println("SNOOZE: LEDs + sound OFF temporarily.");
  isSnoozing = true;
  snoozeStartTime = millis();

  myDFPlayer.stop();
  FastLED.clear();
  FastLED.show();
}

void endSnoozeAndResumeAlarm() {
  Serial.println("SNOOZE END: Resuming alarm.");
  isSnoozing = false;

  fill_solid(leds, NUM_LEDS, endColor);
  FastLED.show();

  myDFPlayer.loop(1);
  sunriseComplete = true;
}

/* =====================================================
   BUTTON
   Short press = snooze
   Long press = stop
   ===================================================== */
void updateButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
    lastButtonReading = reading;
  }

  if (millis() - lastDebounceTime > debounceMs) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      if (stableButtonState == LOW) {
        pressInProgress = true;
        pressStartTime = millis();
      }

      if (stableButtonState == HIGH && pressInProgress) {
        pressInProgress = false;
        unsigned long pressDuration = millis() - pressStartTime;

        if (isPlaying) {
          if (pressDuration >= longPressMs) {
            stopEverything();
          } else {
            startSnooze();
          }
        }
      }
    }
  }
}

/* =====================================================
   SERIAL COMMANDS
   ===================================================== */
void printMenu() {
  Serial.println();
  Serial.println("===== LumaRise Test Commands =====");
  Serial.println("play   -> start sunrise");
  Serial.println("stop   -> stop LEDs + sound");
  Serial.println("0      -> Full Glow pattern");
  Serial.println("1      -> Bottom Up pattern");
  Serial.println("2      -> Center Expand pattern");
  Serial.println("c0     -> Classic Sunrise");
  Serial.println("c1     -> Soft Golden Morning");
  Serial.println("c2     -> Deep Sunset to Sunrise");
  Serial.println("c3     -> Pink Sunrise");
  Serial.println("c4     -> Peach Sky");
  Serial.println("help   -> show this menu");
  Serial.println("==================================");
  Serial.println();
}

void handleSerial() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();

  if (command == "play") {
    if (!isPlaying) {
      startSunrise();
    } else {
      Serial.println("Sunrise already running.");
    }
  }
  else if (command == "stop") {
    if (isPlaying) stopEverything();
    else Serial.println("System already stopped.");
  }
  else if (command == "0" || command == "1" || command == "2") {
    patternId = command.toInt();
    Serial.print("Pattern set to: ");
    Serial.println(patternId);
  }
  else if (command == "c0") {
    setColorCombo(0);
    Serial.println("Color combo set to: Classic Sunrise");
  }
  else if (command == "c1") {
    setColorCombo(1);
    Serial.println("Color combo set to: Soft Golden Morning");
  }
  else if (command == "c2") {
    setColorCombo(2);
    Serial.println("Color combo set to: Deep Sunset to Sunrise");
  }
  else if (command == "c3") {
    setColorCombo(3);
    Serial.println("Color combo set to: Pink Sunrise");
  }
  else if (command == "c4") {
    setColorCombo(4);
    Serial.println("Color combo set to: Peach Sky");
  }
  else if (command == "help") {
    printMenu();
  }
  else {
    Serial.println("Unknown command. Type 'help' for options.");
  }
}

/* =====================================================
   SETUP
   ===================================================== */
void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  mySoftwareSerial.begin(9600);
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Unable to begin DFPlayer Mini.");
    while (true);
  }

  myDFPlayer.volume(25);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  setColorCombo(0);

  Serial.println("LumaRise Student Test Ready!");
  printMenu();
}

/* =====================================================
   LOOP
   ===================================================== */
void loop() {
  updateButton();
  handleSerial();

  if (isSnoozing) {
    if (millis() - snoozeStartTime >= snoozeMs) {
      endSnoozeAndResumeAlarm();
    }
    return;
  }

  if (isPlaying && !sunriseComplete) {
    unsigned long elapsed = millis() - sunriseStartTime;
    float p = (float)elapsed / sunriseDuration;

    if (p >= 1.0f) {
      p = 1.0f;
      showSunrise(p);

      myDFPlayer.loop(1);
      sunriseComplete = true;

      Serial.println("ALARM: Sound started. Short press=snooze, long press=stop.");
    } else {
      showSunrise(p);
    }
  }
}
