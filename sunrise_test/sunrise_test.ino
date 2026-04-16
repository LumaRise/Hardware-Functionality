/*
  LumaRise - Sunrise Pattern Test

  The variable "p" (progress) controls everything:
    p = 0.0 → start of sunrise
    p = 1.0 → end of sunrise

  We use p to:
    - change color (dark → bright)
    - change brightness
    - control patterns (rows, layers, etc.)

  ------------------------------------------------------

  SERIAL COMMANDS:
    0–2   → choose pattern
    start → run sunrise
*/

#include <FastLED.h>

/* ================= LED SETUP ================= */
#define LED_PIN     5
#define NUM_LEDS    64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];   // array that stores all LED colors

/* ================= STUDENT SETTINGS ================= */

// Which pattern to use (0–2)
int patternId = 0;

// How long the sunrise lasts (seconds)
int sunriseDurationSec = 20;

// classic sunrise 
CRGB startColor = CRGB(20, 0, 0);        // beginning (dark red)
CRGB midColor   = CRGB(255, 100, 0);     // middle (orange)
CRGB endColor   = CRGB(255, 255, 220);   // end (warm white)

// Brightness control (0-255)
uint8_t minBrightness = 35;   // ensures light is visible at start
uint8_t maxBrightness = 255;  // set your max brightness

/* ===================================================== */

bool sunriseRunning = false;
unsigned long sunriseStart = 0;

/* ================= XY HELPER =================
   Converts (x, y) position → LED index since LEDs are wired in a zig-zag pattern
*/
int xy(int x, int y) {
  if (y % 2 == 0) return y * 8 + x;
  else            return y * 8 + (7 - x);
}

/* ================= COLOR BLENDING =================
   This function creates the "sunrise color"

   p controls:
   - how far we are into the sunrise
   - what color we should be showing

   We blend:
   start → mid → end
*/
CRGB getSunriseColor(float p) {
  p = constrain(p, 0.0f, 1.0f);

  CRGB c;

  // First half: start → mid
  if (p < 0.5f) {
    float t = p / 0.5f;   // normalize to 0–1

    c.r = startColor.r + (midColor.r - startColor.r) * t;
    c.g = startColor.g + (midColor.g - startColor.g) * t;
    c.b = startColor.b + (midColor.b - startColor.b) * t;
  }

  // Second half: mid → end
  else {
    float t = (p - 0.5f) / 0.5f;

    c.r = midColor.r + (endColor.r - midColor.r) * t;
    c.g = midColor.g + (endColor.g - midColor.g) * t;
    c.b = midColor.b + (endColor.b - midColor.b) * t;
  }

  // Apply brightness scaling
  // Instead of starting at 0, we start at minBrightness
  uint8_t scaledBrightness = map(p * 100, 0, 100, minBrightness, maxBrightness);

  c.nscale8_video(scaledBrightness);

  return c;
}

/* ================= PATTERN 0: FULL GLOW ================= */
//   All LEDs show the same color
void showSunriseFullGlow(float p) {
  CRGB c = getSunriseColor(p);

  fill_solid(leds, NUM_LEDS, c);  // fill entire grid
  FastLED.show();
}

/* ================= PATTERN 1: BOTTOM-UP =================
   Rows turn on from bottom → top

   p controls:
   - how many rows are lit
*/
void showSunriseBottomUp(float p) {
  CRGB c = getSunriseColor(p);

  FastLED.clear();

  int rowsLit = round(p * 8.0f);   // 0–8 rows

  for (int y = 7; y >= 8 - rowsLit; y--) {
    for (int x = 0; x < 8; x++) {
      leds[xy(x, y)] = c;
    }
  }

  FastLED.show();
}

/* ================= PATTERN 2: CENTER 4 EXPAND ================= */
//   Starts with the center 4 LEDs, then expands outward in square layers

void showSunriseCenterExpand(float p) {
  p = constrain(p, 0.0f, 1.0f);
  CRGB c = getSunriseColor(p);

  FastLED.clear();

  // We divide the sunrise into 4 stages
  int stage = floor(p * 4.0f);   // 0 to 4

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {

      // Distance from the center 2x2 block:
      // center block is around x=3,4 and y=3,4
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
//   Chooses which pattern to run

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

/* ================= SERIAL CONTROL ================= */
// Allows you to control LED pattern through Serial Monitor
void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "start") {
    sunriseRunning = true;
    sunriseStart = millis();
    Serial.println("Sunrise started!");
  }

  else if (cmd == "0" || cmd == "1" || cmd == "2") {
    patternId = cmd.toInt();
    Serial.print("Pattern set to: ");
    Serial.println(patternId);
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(9600);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);

  // Global brightness cap (extra safety)
  FastLED.setBrightness(50);

  FastLED.clear();
  FastLED.show();

  Serial.println("Ready!");
}

/* ================= LOOP ================= */
void loop() {
  handleSerial();

  if (sunriseRunning) {

    // Calculate how long sunrise has been running
    float elapsed = (millis() - sunriseStart) / 1000.0;

    // Convert into progress (0 → 1)
    float p = elapsed / sunriseDurationSec;

    if (p >= 1.0f) {
      p = 1.0f;
      showSunrise(p);
      sunriseRunning = false;
      Serial.println("Sunrise complete!");
    } else {
      showSunrise(p);
    }
  }
}
