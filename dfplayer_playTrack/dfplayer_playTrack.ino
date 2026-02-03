#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

SoftwareSerial mySoftwareSerial(6, 7); // RX, TX
DFRobotDFPlayerMini myDFPlayer;
bool isPlaying = false;

void setup() {
  mySoftwareSerial.begin(9600);
  Serial.begin(9600);

  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println("Unable to begin DFPlayer Mini.");
    while (true);
  }

  Serial.println("DFPlayer Mini ready. Type 'play' or 'stop' in Serial Monitor.");
  myDFPlayer.volume(10); // Set volume (0–30)
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command == "play" && !isPlaying) {
      myDFPlayer.loop(1);  // Loop track 0001.mp3
      isPlaying = true;
      Serial.println("Looping track 1...");
    }
    else if (command == "stop" && isPlaying) {
      myDFPlayer.stop();
      isPlaying = false;
      Serial.println("Playback stopped.");
    }
  }
}
