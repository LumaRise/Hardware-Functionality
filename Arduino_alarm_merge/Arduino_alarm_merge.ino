#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

/* ===================== DFPLAYER SETUP ===================== */
SoftwareSerial mySoftwareSerial(6, 7); // RX, TX
DFRobotDFPlayerMini myDFPlayer;

// Keeps track of whether sound is currently playing
bool isPlaying = false;

/* ===================== ESP32 DATA RECEIVED  ===================== */
String espTime = "";    // Current time string from ESP32
String espAlarm = "";   // Alarm time string from ESP32

/* ===================== HELPER: PLAY TRACK ===================== */
void startAlarm() {
  if (!isPlaying) {
    myDFPlayer.loop(1);   // Loop track 0001.mp3
    isPlaying = true;
    Serial.println("ALARM: PLAY (loop track 3)");
  }
}

/* ===================== HELPER: STOP TRACK ===================== */
void stopAlarm() {
  if (isPlaying) {
    myDFPlayer.stop();
    isPlaying = false;
    Serial.println("ALARM: STOP");
  }
}

void setup() {
 /* --------- SERIAL SETUP --------- */
  Serial.begin(9600);     // Open Arduino Serial Monitor
  Serial1.begin(9600);    // Must match ESP32 Serial1 baud rate

  // Start DFPlayer serial communication
  mySoftwareSerial.begin(9600);

  Serial.println("Arduino booting...");

  /* --------- DFPLAYER INITIALIZATION --------- */
  // If the DFPlayer cannot be detected, stop the program
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Unable to begin DFPlayer Mini.");
    Serial.println("Check wiring and SD card.");
    while (true);  // Infinite loop = stop everything
  }

  // Set volume (0–30)
  myDFPlayer.volume(10);

  Serial.println("DFPlayer ready.");
  Serial.println("Waiting for messages from ESP32...");
}


void loop() {
  /* ===================== READ MSGS FROM ESP32 ===================== */
  if (Serial1.available()) {

    // Read one full line
    String message = Serial1.readStringUntil('\n');
    message.trim();  // Remove extra spaces/newlines

    // Print everything received 
    Serial.println("RX: " + message);

    /* ---------------- TIME MESSAGE ---------------- */
    if (message.startsWith("TIME, ")) {

      // Remove "TIME," from the front
      espTime = message.substring(5);
      espTime.trim();
    }

    /* ---------------- ALARM MESSAGE ---------------- */
    else if (message.startsWith("ALARM, ")) {

      // Remove "ALARM," from the front
      espAlarm = message.substring(6);
      espAlarm.trim();

      Serial.println("Alarm set to: " + espAlarm);

      // Reset isPlaying so alarm can trigger again later
      isPlaying = false;
    }


    /* ---------------- COMMAND MESSAGE ---------------- */
    else if (message.startsWith("CMD,")) {

      // Extract the command after "CMD,"
      String cmd = message.substring(4);
      cmd.trim();

      /* ----- PLAY COMMAND ----- */
      if (cmd == "PLAY") {
        startAlarm();
      }

      /* ----- STOP COMMAND ----- */
      else if (cmd == "STOP") {
        stopAlarm();
      }

      /* ----- SNOOZE COMMAND ----- */
      else if (cmd == "SNOOZE") {
        // For now: Snooze just stops the sound.
        // ESP32 handles the snooze timer.
        stopAlarm();
        Serial.println("SNOOZE received (sound stopped)");
      }
    }
  }
}