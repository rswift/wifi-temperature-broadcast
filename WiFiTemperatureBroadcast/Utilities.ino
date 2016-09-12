/* 
 * Any utilities functions go here...
 *  
 */

#if defined(ERROR_LED) || defined(STATUS_LED) || defined(READING_LED) // minimise the code where possible
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
#endif
