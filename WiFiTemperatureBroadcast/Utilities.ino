/* 
 * Any utilities functions go here...
 *  
 */

// does this need to be explained? hopefully not...
void flashLED(int ledToFlash, int flashCount) {
  do {
    if (flashCount > 0) {
      delay(123);
    }
    digitalWrite(ledToFlash, HIGH);
    delay(123);
    digitalWrite(ledToFlash, LOW);
  } while (--flashCount > 0);
}
