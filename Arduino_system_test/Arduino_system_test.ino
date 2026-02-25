#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <FastLED.h>

// ----- LED Ring Setup -----
#define LED_PIN     5
#define NUM_LEDS    64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define BRIGHTNESS  50

CRGB leds[NUM_LEDS];

// ----- DFPlayer Mini Setup -----
SoftwareSerial mySoftwareSerial(6, 7); // RX (pin 6), TX (pin 7)
DFRobotDFPlayerMini myDFPlayer;

// ----- Button Setup -----
#define BUTTON_PIN  2   

// ----- Timing + Press Settings -----
const unsigned long sunriseDuration = 60000;      // 60 seconds
const unsigned long longPressMs     = 900;        // hold >= 0.9s to STOP
const unsigned long debounceMs      = 40;         // debounce
const unsigned long snoozeMs        = 10000;      // <-- snooze duration (10s for testing; change to e.g. 300000 for 5 min)

// ----- State -----
bool isPlaying = false;              // overall system active
bool sunriseComplete = false;        // sunrise finished & audio started
unsigned long sunriseStartTime = 0;

bool isSnoozing = false;
unsigned long snoozeStartTime = 0;

// ----- Button tracking -----
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;

bool pressInProgress = false;
unsigned long pressStartTime = 0;

// ---------- Helpers ----------
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
  if (!sunriseComplete) {
    // If they snooze before audio begins, you can choose to ignore
    // or still snooze. We'll allow snooze anyway.
  }

  Serial.println("SNOOZE: LEDs + sound OFF (temporarily).");
  isSnoozing = true;
  snoozeStartTime = millis();

  myDFPlayer.stop();
  FastLED.clear();
  FastLED.show();
}

void endSnoozeAndResumeAlarm() {
  Serial.println("SNOOZE END: Resuming alarm (light + sound).");
  isSnoozing = false;

  // Resume to "alarm on" state (final warm color + looping track)
  fill_solid(leds, NUM_LEDS, CHSV(32, 200, 255));
  FastLED.show();
  myDFPlayer.loop(1);
  sunriseComplete = true;
}

// ---------- Button update ----------
void updateButton() {
  bool reading = digitalRead(BUTTON_PIN);

  // Debounce
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
    lastButtonReading = reading;
  }

  if (millis() - lastDebounceTime > debounceMs) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      // Button pressed (active LOW)
      if (stableButtonState == LOW) {
        pressInProgress = true;
        pressStartTime = millis();
      }

      // Button released
      if (stableButtonState == HIGH && pressInProgress) {
        pressInProgress = false;
        unsigned long pressDuration = millis() - pressStartTime;

        // Only respond if the alarm system is active
        if (isPlaying) {
          if (pressDuration >= longPressMs) {
            // Long press -> STOP
            stopEverything();
          } else {
            // Short press -> SNOOZE (only if alarm has started / active)
            startSnooze();
          }
        }
      }
    }
  }
}

void setup() {
  Serial.begin(9600);

  // Button: use internal pullup (button to GND)
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // DFPlayer
  mySoftwareSerial.begin(9600);
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Unable to begin DFPlayer Mini.");
    while (true);
  }
  myDFPlayer.volume(25);
  Serial.println("DFPlayer Mini ready. Type 'play' or 'stop'.");

  // LEDs
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();
}

void loop() {
  // Always check button
  updateButton();

  // Serial commands
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command == "play" && !isPlaying) {
      startSunrise();
    }

    if (command == "stop" && isPlaying) {
      stopEverything();
    }
  }

  // Snooze timer
  if (isSnoozing) {
    if (millis() - snoozeStartTime >= snoozeMs) {
      endSnoozeAndResumeAlarm();
    }
    return; // while snoozing, don’t run sunrise logic
  }

  // Sunrise simulation
  if (isPlaying && !sunriseComplete) {
    unsigned long elapsed = millis() - sunriseStartTime;

    if (elapsed >= sunriseDuration) {
      fill_solid(leds, NUM_LEDS, CHSV(32, 200, 255));
      FastLED.show();

      myDFPlayer.loop(1);
      sunriseComplete = true;

      Serial.println("ALARM: Sound started. Button short=snooze, long=stop.");
    } else {
      float progress = (float)elapsed / sunriseDuration;
      uint8_t hue = 0 + progress * (32 - 0);
      uint8_t brightness = 30 + progress * (255 - 30);

      fill_solid(leds, NUM_LEDS, CHSV(hue, 255, brightness));
      FastLED.show();
    }
  }
}