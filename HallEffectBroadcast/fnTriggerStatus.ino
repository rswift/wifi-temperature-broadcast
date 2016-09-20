/*
 * Display the Trigger Status in the top 16 rows
 * 
 * The layout is simple, text left aligned, battery status right. The status message cycles through
 * Ambient Temperature, Relative Humidity and Status
 * 
 */

const char statusLabel[] = "Status:";
const byte statusLabelStartX = 1, statusLabelStartY = 5;
const byte statusStartX = statusLabelStartX + (sizeof(statusLabel) * 5) + 2, statusStartY = statusLabelStartY, statusLabelLength = sizeof(statusLabel), maximumStatusMessageLength = 8;
const byte batteryStartY = 2;


void setTriggerStatusText(char *statusText, boolean includeLabel) {
  if (debugLogging && verboseLogging) { Serial.print(F("About to render trigger status text: ")); Serial.println(statusText); }

  // clear the status bar, saves messing with things like writing spaces
  display.fillRect(0, statusLabelStartY, batteryStartX - 1, 10, BLACK);
  display.setCursor(statusLabelStartX, statusLabelStartY);
  display.setTextColor(WHITE, BLACK);
  byte statusTextLength = statusLabelLength + maximumStatusMessageLength;
  if (includeLabel) {
    display.print(statusLabel);
    display.setCursor(statusStartX, statusStartY);
    statusTextLength = maximumStatusMessageLength;
  }
  display.printf("%s", statusText);
  display.display(); delay(1);
}

// quite simply the value of ESP.getVcc() divided by the measured battery voltage for a range of values
const float batteryConversion = 965.8;

// convenience function
void drawTriggerBattery() {
  drawBattery(ESP.getVcc() / batteryConversion, triggerBatteryY);
}

void initaliseTriggerDisplay() {
  display.drawFastHLine(0, 0, display.width(), WHITE);
  display.drawFastHLine(0, 15, display.width(), WHITE);
  drawTriggerBattery();
}
