/* 
 * Any utilities functions go here...
 *  
 */

void flashLED(int ledToFlash, int flashCount) {
  do {
    if (flashCount > 0) {
      delay(77);
    }
    digitalWrite(ledToFlash, HIGH);
    delay(77);
    digitalWrite(ledToFlash, LOW);
  } while (--flashCount > 0);
}
