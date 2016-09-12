/* 
 * Functions to create the messages for broadcast to the various hosts.
 * 
 * Only one parameter should be passed in:
 * 
 *  - broadcastTemperatureCelsius, the temperature in celsius
 *  
 */

// Trivial function to format the data to broadcast to listners...
String formatBroadcastMessage(double broadcastTemperatureCelsius) {
  // possibly slower, but the overhead isn't a major concern in relation to the benefit
  // https://github.com/bblanchon/ArduinoJson/wiki/FAQ#what-are-the-differences-between-staticjsonbuffer-and-dynamicjsonbuffer
  DynamicJsonBuffer jsonBuffer;
  JsonObject& jsonResponse = jsonBuffer.createObject();

  JsonArray& readings = jsonResponse.createNestedArray("readings");
  JsonObject& reading = readings.createNestedObject();
  reading["reading"] = double_with_n_digits(broadcastTemperatureCelsius, 2);
  reading["scale"] = "C";
  reading["probeName"] = probeName;
  reading["probeType"] = probeType;
  reading["probeSubType"] = temperatureProbeType;

  JsonObject& systemInformation = jsonResponse.createNestedObject("systemInformation");
  systemInformation["VCC"] = double_with_n_digits((ESP.getVcc() / 1024.0), 3);

  if (debugLogging) {
    JsonObject& debugData = jsonResponse.createNestedObject("debugData");
    debugData["minimumInternal"] = double_with_n_digits(minimumInternal, 3);
    debugData["maximumInternal"] = double_with_n_digits(maximumInternal, 3);
    debugData["indexRolledOver"] = indexRolledOver;
    debugData["rollingAveragePosition"] = rollingAveragePosition;
    int celsiusRollingAverageSize = sizeof(celsiusRollingAverage) / sizeof(celsiusRollingAverage[0]);
    debugData["celsiusRollingAverageSize"] = celsiusRollingAverageSize;
    JsonArray& rawCelsiusReadingValues = debugData.createNestedArray("rawCelsiusReadingValues");
    for (int i = 0; i < celsiusRollingAverageSize; i++) {
      rawCelsiusReadingValues.add(celsiusRollingAverage[i]);
    }
    debugData["DRUM_ROTATION_SPEED"] = DRUM_ROTATION_SPEED;
    debugData["PROBE_RATE"] = PROBE_RATE;
    debugData["probeReadingErrorCount"] = probeReadingErrorCount;
    debugData["chipId"] = ESP.getChipId();
    Serial.print(F("JSON Object: ")); jsonResponse.prettyPrintTo(Serial); Serial.println();
  }

  String broadcastData;
  jsonResponse.printTo(broadcastData);
  return broadcastData;
}

// see https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs
String formatRoastmasterDatagramProtocolMessage(double broadcastTemperatureCelsius) {
  DynamicJsonBuffer jsonTemperatureBuffer;
  JsonObject& rdpTemperatureTransmission = jsonTemperatureBuffer.createObject();
  rdpTemperatureTransmission[RDPKey_Version] = RDPValue_Version;
  rdpTemperatureTransmission[RDPKey_Serial] = probeName;
  rdpTemperatureTransmission[RDPKey_Epoch] = rdpEpoch++;
  JsonArray& rdpPayload = rdpTemperatureTransmission.createNestedArray(RDPKey_Payload);
  JsonObject& rdpReading = rdpPayload.createNestedObject();
  rdpReading[RDPKey_Channel] = 1;
  rdpReading[RDPKey_EventType] = (int)RDPEventType_Temperature;
  rdpReading[RDPKey_Value] = double_with_n_digits(broadcastTemperatureCelsius, 2);
  rdpReading[RDPKey_Meta] = (int)RDPMetaType_BTTemp;

  if (debugLogging) {
    Serial.print(F("JSON Object: "));
    rdpTemperatureTransmission.prettyPrintTo(Serial);
    Serial.println();
  }

  String broadcastData;
  rdpTemperatureTransmission.printTo(broadcastData);
  return broadcastData;
}
