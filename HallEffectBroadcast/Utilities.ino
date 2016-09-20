/* Any utilities functions go here... */

/*
 * Draw a battery and overlay a voltage to one decimal place
 */
void drawBattery(float voltage, byte topRow) {
  // draw the battery with terminal, all x,y from 0,0
  display.fillRect(batteryStartX, topRow+3, 2, 6, WHITE);
  display.fillRect(batteryStartX+2, topRow, 27, 12, WHITE);
  display.setCursor(batteryStartX+4, topRow+2);
  display.setTextColor(BLACK, WHITE);
  display.print(String(voltage, 1)); display.print("v");
  display.display(); delay(1);
  display.setTextColor(WHITE, BLACK);
}

void flashLED(int ledToFlash, int flashCount) {
  if (ledEnabled) {
    do {
      if (flashCount > 0) {
        delay(77);
      }
      digitalWrite(ledToFlash, HIGH);
      delay(77);
      digitalWrite(ledToFlash, LOW);
    } while (--flashCount > 0);
  }
}

