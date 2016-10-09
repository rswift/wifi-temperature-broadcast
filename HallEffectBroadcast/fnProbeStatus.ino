/*
 * 
 */

const char degree = '\xF8';
const char eacute = '\x82';
void updateProbeStatus() {

  int probePacketLength = probeUdp.parsePacket();
  if (probePacketLength > 0) {
    // Broadcast packet received
    char probePacket[probePacketLength + 1];
    memset(probePacket, 0, probePacketLength + 1);
    probeUdp.read(probePacket, probePacketLength);
    if (debugLogging) { Serial.print(F("Probe broadcast bytes received via ")); Serial.print(probeUdp.destinationIP()); Serial.print(F(": ")); Serial.println(probePacket); }

    DynamicJsonBuffer jsonBuffer;
    JsonObject& probeJson = jsonBuffer.parseObject(probePacket);
    JsonArray& probeReadings = probeJson["readings"].as<JsonArray>();

    // currently only interested in bean mass probe readings
    for (int i = 0; i < probeReadings.size(); i++) {
      int probeType = probeReadings[i]["probeType"].as<int>();
      int probeSubType = probeReadings[i]["probeSubType"].as<int>();
      const char *reading = probeReadings[i]["reading"].as<char *>();
      const char *scale = probeReadings[i]["scale"].as<char *>();
      const char *probeName = probeReadings[i]["probeName"].as<char *>();
      if (probeType == 0 && probeSubType == 0) {
        setTriggerStatusText("Got BMR!", true);
        // got a bean mass reading
        clearProbeDisplay();
        char text[display.width()/5];
//        sprintf(text, "%s", probeName);
//        setProbeMessageText(3, text);
        sprintf(text, "Bean Mass: %s%c%s", reading, degree, scale);
        setProbeMessageText(4, text);
      }
    }

    float probeVcc = probeJson["systemInformation"]["VCC"].as<float>();
    probeUdp.flush();
    updateOnboardSensorStatus();

    // rely on this function executing the display.display();
    drawBattery(probeVcc, probeBatteryY);
  } else {
    // this is to update the display sensor in the event of no UDP packet
    updateOnboardSensorStatus();
    display.display(); delay(1);
  }
}

const byte startRow = 20, rowHeight = 8+1+1+1;
void setProbeMessageText(byte row, char *text) {
  display.setCursor(0, startRow + ((row -1) * rowHeight));
  display.setTextColor(WHITE, BLACK);
  display.print(text);
}

void clearProbeDisplay() {
  display.fillRect(0, 18, display.width(), 62 - 18, BLACK);
}

void initaliseProbeDisplay() {
  display.drawFastHLine(0, 16, display.width(), WHITE);
  display.drawFastHLine(0, 63, display.width(), WHITE);
}

void updateOnboardSensorStatus() {
  if (shtSensorAvailable) {
    char text[display.width()/5];
    char floater[4+1];
    dtostrf(sensorReadings.temperature, 4, 1, floater);
    sprintf(text, "Ambient: %s%cC", floater, degree);
    setProbeMessageText(1, text);
    dtostrf(sensorReadings.humidity, 4, 1, floater);
    sprintf(text, "Rel. Humidity: %s%%", floater);
    setProbeMessageText(2, text);
  }
}
