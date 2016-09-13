/* 
 * Function separated from main code for easier readability, but it manipulates variables in the main
 * file WiFiTemperatureBroadcast.ino
 * 
 */

void broadcastReadings() {

  // if there was a problem reading the probes or there is no data to broadcast, alert and return
  if ((rollingAveragePosition == 0 && !indexRolledOver) || probeReadingError) {
    if (debugLogging) { Serial.print(F("Not going to broadcast readings, rollingAveragePosition is ")); Serial.print(rollingAveragePosition); Serial.print(F(" and probeReadingError is ")); Serial.println(probeReadingError); }
    flashLED(errorLed, 3);
    return;
  }

  // calculate the rolling average and broadcast it to anyone who cares to listen...
  double broadcastTemperatureC = 0.0;
  double broadcastTemperatureF = 0.0;

  // determine how much of the array to read
  int endOfIndex = rollingAveragePosition;
  if (indexRolledOver) {
    // fabulous, read the whole thing
    endOfIndex = ROLLING_AVERAGE_COUNT;
  }

  for (int i = 0; i < endOfIndex; i++) {
    if (debugLogging && verboseLogging) { Serial.printf("Reading celsiusRollingAverage[%d]=", i); Serial.println(celsiusRollingAverage[i]); }
    broadcastTemperatureC += celsiusRollingAverage[i];
  }

  broadcastTemperatureC = broadcastTemperatureC / ((double)endOfIndex); // adding 0.0 forces the int to be cast, probably a crappy way of doing it...

  String broadcastMessage = formatBroadcastMessage(broadcastTemperatureC);
  byte broadcastBytes[broadcastMessage.length() + 1];
  broadcastMessage.getBytes(broadcastBytes, broadcastMessage.length() + 1);

  String roastmasterMessage = formatRoastmasterDatagramProtocolMessage(broadcastTemperatureC);
  byte roastmasterBroadcastBytes[roastmasterMessage.length() + 1];
  roastmasterMessage.getBytes(roastmasterBroadcastBytes, roastmasterMessage.length() + 1);
  
  if (debugLogging && verboseLogging) {
    Serial.print(F("minimumInternal=")); Serial.println(minimumInternal); Serial.print(F("maximumInternal=")); Serial.println(maximumInternal);
    Serial.printf("Broadcast [%s] to UDP port %d via ", broadcastBytes, udpRemotePort); Serial.println(broadcastAddress);
    if (gotRDPServer) {
      Serial.printf("Broadcast [%s] to RDP port %d via ", roastmasterBroadcastBytes, rdpRemotePort); Serial.println(rdpIPAddress);
    }
  }

  // check the WiFi connection, then send the datagram
  if (WiFi.status() == WL_CONNECTED) {
    if (debugLogging) { Serial.println(F("Broadcasting...")); }

    // start with my own
    if (brdUdp.beginPacket(broadcastAddress, udpRemotePort) == 1) {
//    if (brdUdp.beginPacketMulticast(multicastAddress, udpRemotePort, WiFi.localIP()) == 1) {
      brdUdp.write(broadcastBytes, broadcastMessage.length() + 0);
      byte udpEndPacket = brdUdp.endPacket();
      if (udpEndPacket != 1) {
        if (debugLogging) { Serial.print(F("ERROR! My UDP broadcast failed with code: ")); Serial.println(udpEndPacket); }
        flashLED(errorLed, 3);
      } else {
        flashLED(statusLed, 1);
      }
    } else {
      if (debugLogging) { Serial.print(F("ERROR! My UDP broadcast setup failed!")); }
      flashLED(errorLed, 3);
    } //end of my broadcast handling

    // the gotRDPServer test is first to ensure that the beginPacket isn't triggered on false...
    if (gotRDPServer && rdpUdp.beginPacket(rdpIPAddress, rdpRemotePort) == 1) {
      rdpUdp.write(roastmasterBroadcastBytes, roastmasterMessage.length() + 0);
      byte rdpEndPacket = rdpUdp.endPacket();
      if (rdpEndPacket != 1) {
        if (debugLogging) { Serial.print(F("ERROR! RDP UDP broadcast failed with code: ")); Serial.println(rdpEndPacket); }
        flashLED(errorLed, 3);
      } else {
        flashLED(statusLed, 1);
      }
    } else {
      // it is only an error if gotRDPServer is true
      if (debugLogging && gotRDPServer) {
        Serial.print(F("ERROR! RDP UDP broadcast setup failed!"));
        flashLED(errorLed, 3);
      }
    } // end of the RDP broadcast
  
  } else {
    // a problem occurred! need to implement something better to advertise the fact...
    if (debugLogging) { Serial.print(F("ERROR! WiFi status showing as not connected: ")); Serial.println(WiFi.status()); }
    flashLED(errorLed, 5);
  }

  // reset the fields for readProbes()
  rollingAveragePosition = 0;
  indexRolledOver = false;
}
