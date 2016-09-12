/* 
 * Function separated from main code for easier readability, but it manipulates variables in the main
 * file WiFiTemperatureBroadcast.ino
 * 
 */

void readProbes() {
  
  double internalTemperature = thermocouple.readInternal();
  double temperatureCelsius = thermocouple.readCelsius();

  // if the sensor can't be read correctly, wait for a cycle and try again
  #ifdef ERROR_LED
    digitalWrite(ERROR_LED, LOW); // turn off the LED
  #endif
  if (isnan(internalTemperature) || isnan(temperatureCelsius)) {
    probeReadingError = true;
    probeReadingErrorCount++;
    if (debugLogging) { Serial.println("Something is wrong with thermocouple! Is it grounded?"); }
    #ifdef ERROR_LED
      digitalWrite(ERROR_LED, HIGH); // turn on the error indication LED
    #endif
    return; 
  } else {
      probeReadingError = false;
      #ifdef READING_LED
        flashLED(READING_LED, 1);
    #endif
  }

  // track minimum and maximum
  if (internalTemperature < minimumInternal) {
    minimumInternal = internalTemperature;
  }
  if (internalTemperature > maximumInternal) {
    maximumInternal = internalTemperature;
  }

  if (debugLogging) { Serial.print(F("Internal temperature is ")); Serial.print(internalTemperature); Serial.print(F("°C and the probe is reading (linearised) ")); Serial.print(temperatureCelsius); Serial.print(F("°C (")); Serial.print(lineariseTemperature(internalTemperature, temperatureCelsius)); Serial.print(F("°C) and readError is: ")); Serial.println(thermocouple.readError()); }

  // some basic protection logic just in case the broadcast hasn't happened in a timely manner...
  if (rollingAveragePosition >= ROLLING_AVERAGE_COUNT) {
    if (debugLogging) { Serial.println("ERROR! resetting rolling average index to zero - broadcast can't have happened at the correct time"); }
    rollingAveragePosition = 0;
    indexRolledOver = true;
  }

  // write the temperature data values to the rolling average array
  celsiusRollingAverage[rollingAveragePosition] = lineariseTemperature(internalTemperature, temperatureCelsius);

  if (debugLogging && verboseLogging) { Serial.print("Setting celsiusRollingAverage["); Serial.print(rollingAveragePosition); Serial.print("]="); Serial.println(celsiusRollingAverage[rollingAveragePosition]); }

  // update the array offset for the next capture before looping again
  rollingAveragePosition++;
}
