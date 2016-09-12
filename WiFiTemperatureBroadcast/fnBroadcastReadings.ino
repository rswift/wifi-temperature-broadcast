/* 
 * Function separated from main code for easier readability, but it manipulates variables in the main
 * file WiFiTemperatureBroadcast.ino
 * 
 */

void broadcastReadings() {

  // if there was a problem reading the probes or there is no data to broadcast, alert and return
  if ((rollingAveragePosition == 0 && !indexRolledOver) || probeReadingError) {
    if (debugLogging) { Serial.print(F("Not going to broadcast readings, rollingAveragePosition is ")); Serial.print(rollingAveragePosition); Serial.print(F(" and probeReadingError is ")); Serial.println(probeReadingError); }
    #ifdef ERROR_LED
      flashLED(ERROR_LED, 3);
    #endif
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
  broadcastTemperatureF = (broadcastTemperatureC * (9.0/5.0)) + 32.0;

  String broadcastMessage = formatBroadcastMessage(broadcastTemperatureC); //, broadcastTemperatureF);
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

     // the gotRDPServer test is first to ensure that the beginPacket isn't triggered on false...
    if (brdUdp.beginPacket(broadcastAddress, udpRemotePort) == 1 && (gotRDPServer && rdpUdp.beginPacket(rdpIPAddress, rdpRemotePort) == 1)) {
//    if (brdUdp.beginPacketMulticast(multicastAddress, udpRemotePort, WiFi.localIP()) == 1) {
      brdUdp.write(broadcastBytes, broadcastMessage.length() + 0);
      rdpUdp.write(roastmasterBroadcastBytes, roastmasterMessage.length() + 0);

      byte udpEndPacket = brdUdp.endPacket();
      byte rdpEndPacket = rdpUdp.endPacket();
      if (udpEndPacket != 1 || rdpEndPacket != 1) {
        if (debugLogging) { Serial.printf("ERROR! UDP broadcast failed! (%d:%d)", udpEndPacket, rdpEndPacket); }
        #ifdef ERROR_LED
          flashLED(ERROR_LED, 3);
        #endif
      #ifdef STATUS_LED
      } else {
        flashLED(STATUS_LED, 1);
      #endif
      }
    } else {
      if (debugLogging) { Serial.print(F("ERROR! UDP broadcast setup failed!")); }
      #ifdef ERROR_LED
        flashLED(ERROR_LED, 3);
      #endif
    }
  } else {
    // a problem occurred! need to implement something better to advertise the fact...
    if (debugLogging) {
      Serial.print(F("ERROR! WiFi status showing as not connected: "));
      Serial.println(WiFi.status());
    }
    #ifdef ERROR_LED
      flashLED(ERROR_LED, 5);
    #endif
  }

  // reset the fields for readProbes()
  rollingAveragePosition = 0;
  indexRolledOver = false;
}
