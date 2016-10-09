/* 
 * Function separated from main code for easier readability, but it manipulates variables in the main
 * file WiFiTemperatureBroadcast.ino
 * 
 */

void readProbes() {

//  max31855readings readings = thermocouple.readTemperatures();
  max31855readings readings;
  readings.internal = thermocouple.readInternal();
  readings.celsius = thermocouple.readCelsius();

  // if the sensor can't be read correctly, wait for a cycle and try again
  digitalWrite(errorLed, LOW); // turn off the LED
  if (isnan(readings.internal) || isnan(readings.celsius)) {
    probeReadingError = true;
    probeReadingErrorCount++;
    if (debugLogging) { Serial.println("Something is wrong with thermocouple! Is it grounded?"); }
    digitalWrite(errorLed, HIGH); // turn on the error indication LED
    return; 
  } else {
      probeReadingError = false;
/*      flashLED(readingLed, 1); */
  }

  // track minimum and maximum
  if (readings.internal < minimumInternal) {
    minimumInternal = readings.internal;
  }
  if (readings.internal > maximumInternal) {
    maximumInternal = readings.internal;
  }

  if (debugLogging) { Serial.print(F("Internal temperature is ")); Serial.print(readings.internal); Serial.print(F("°C and the probe is reading (linearised) ")); Serial.print(readings.celsius); Serial.print(F("°C (")); Serial.print(lineariseTemperature(readings.internal, readings.celsius)); Serial.println(F("°C)")); }

  // some basic protection logic just in case the broadcast hasn't happened in a timely manner...
  if (rollingAveragePosition >= ROLLING_AVERAGE_COUNT) {
    if (debugLogging) { Serial.println("ERROR! resetting rolling average index to zero - broadcast can't have happened at the correct time"); }
    rollingAveragePosition = 0;
    indexRolledOver = true;
  }

  // write the temperature data values to the rolling average array
  long microsPreLinearise = micros();
  celsiusRollingAverage[rollingAveragePosition] = lineariseTemperature(readings.internal, readings.celsius);
  long microsPostLinearise = micros();

  if (debugLogging && verboseLogging) { Serial.print("Setting celsiusRollingAverage["); Serial.print(rollingAveragePosition); Serial.print("]="); Serial.println(celsiusRollingAverage[rollingAveragePosition]); }

  // update the array offset for the next capture before looping again
  rollingAveragePosition++;
}
