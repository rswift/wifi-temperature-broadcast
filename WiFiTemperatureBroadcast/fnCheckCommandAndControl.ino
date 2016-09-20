/* 
 * Function separated from main code for easier readability, but it manipulates variables in the main
 * file WiFiTemperatureBroadcast.ino
 * 
 */

void checkForCommandAndControlMessage() {
  int cncPacketLength = cncUdp.parsePacket();
  if (cncPacketLength > 0) {
    // Command & Control packet received
    char cncPacket[cncPacketLength + 1];
    memset(cncPacket, 0, cncPacketLength + 1);
    cncUdp.read(cncPacket, cncPacketLength);
    if (debugLogging) { Serial.print(F("Command & Control bytes received via ")); Serial.print(cncUdp.destinationIP()); Serial.print(F(": ")); Serial.println(cncPacket); }

    DynamicJsonBuffer jsonBuffer;
    JsonObject& cncJson = jsonBuffer.parseObject(cncPacket);
    String cncCommand = cncJson["command"].as<String>();
    //    String cncContext = cncJson["context"].as<String>();
    if (cncCommand == "readProbes") { // somewhat clumsy, but it'll do for the time being
      // switch 'activated', start reading temperatures
      digitalWrite(readingLed, HIGH);
      shouldReadProbes = true;
      shouldBroadcast = false;
      haveBroadcastSinceRead = false;
    } else if (cncCommand == "broadcastReadings") {
      // switch 'deactivated', so broadcast readings
      digitalWrite(readingLed, LOW);
      shouldReadProbes = false;
      shouldBroadcast = true;
    }
    cncUdp.flush();
  }
}
