/*
 * This is a simple application, firmly inspired by Roasthacker to capture and broadcast  
 * the bean mass temperature from my Gene Café CBR-101 coffee roaster. The ultimate goal
 * is to feed these temperatures into the Roastmaster Data Logger for hands-free temperature
 * capture directly on the iPad...
 * 
 * It has been written and tested on the Adafruit HUZZAH ESP8266 with the Adafruit MAX31855 
 * plus K-Type Thermocouple. It is therefore based on the many, excellent example sketches
 * available within the Arduino IDE and elsewhere...
 * 
 * The payload is formatted as JSON, but is described in a method so can be changed as desired
 * 
 * This stuff really is amazing, when I think how big the 300 baud modems I used to provide
 * remote support for customers around the globe, to be able to get this much capability
 * into a matchbox is phenomenal :)
 * 
 * This code is issued under the "Bill and Ted - be excellent to each other" licence (which
 * has no content, just, well, be excellent to each other), however, I have included the licence
 * files from my original starting file (serialthermocouple.ino) at the bottom of this sketch...
 * 
 * Usage:
 *  - set the probeName, WiFi and UDP broadcast port settings
 *  - ensure the MAX31855 pins are assigned correctly
 *  - tinker with the poll rates, debug/verbose and baud rate values as you prefer
 *  - flash the ESP8266
 *  - monitor your network for UDP datagrams sent to the port specified by udpRemotePort
 *  - and/or consume the datagram in a suitable application
 * 
 * ToDo:
 *  - understand the impact of fluctuations of the cold junction temperature; OR just solder the thermocouple wire to the MAX31855, solves the problem!
 *  - dynamic WiFi settings to permit the client application to govern settings such as
 *    SSID & password, poll/broadcast rates etc. and store in EEPROM
 *  - test on other boards
 *  - simplify the code, maybe separate out although it is handy having it all in a single file
 *  - add a tilt switch to mean that the temperature readings are taken when the probe is actually in the bean mass, then broadcast afterwards
 *  - build a small server element into the code to receive instructions (broadcast now, configuration etc.)
 * 
 * Some links:
 *  - Roasthacker: http://roasthacker.com/?p=529 & http://roasthacker.com/?p=552
 *  - Adafruit ESP8266: https://www.adafruit.com/products/2471
 *  - Adafruit MAX31855: https://www.adafruit.com/products/269 & https://cdn-shop.adafruit.com/datasheets/MAX31855.pdf
 *  - Using a thermocouple: https://learn.adafruit.com/thermocouple/using-a-thermocouple
 *  - Calibration: https://learn.adafruit.com/calibrating-sensors/maxim-31855-linearization
 *  - TCP dump or Wireshark for packet capture: http://www.tcpdump.org/tcpdump_man.html or https://www.wireshark.org/
 *  - Timer: http://www.switchdoc.com/2015/10/iot-esp8266-timer-tutorial-arduino-ide/
 *  
 *  Robert Swift - May 2016.
 */

#include <Arduino.h>
#include "Adafruit_MAX31855.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <float.h> // provides min/max double values
extern "C" {
#include "user_interface.h" // needed for the timer
}

// Commenting the following two line in will enable basic logging, VERBOSE is, well, verbose, but does require DEBUG to be set to do anything meaningful
#define DEBUG
//#define VERBOSE

// put this up near the top, to make it easy to spot for those not using the same baud rate as me for their FTDI Friend...
const unsigned int baudRate = 115200;

// comment out to remove all startup delay, I found it was necessary to let the board get its together
#define STARTUP_DELAY 1500

// This is the name that will be included in the broadcast message
const char probeName[] = "Gene Café Bean Mass";

// Set the WiFi and network details
const char networkSSID[] = "SSID";
const char wifiPassword[] = "password";
IPAddress localIPAddress, subnetMask, broadcastAddress;

// no reason, just the number of the temperature amp board
const int udpRemotePort = 31855;

// Define the pin mapping from the MAX31855 interface to the microcontroller (https://learn.adafruit.com/thermocouple/using-a-thermocouple)
#define DO 2 // DO (data out) is an output from the MAX31855 (input to the microcontroller) which carries each bit of data
#define CS 4 // CS (chip select) is an input to the MAX31855 (output from the microcontroller) and tells the chip when its time to read the thermocouple and output more data
#define CLK 5 // CLK (clock) is an input to the MAX31855 (output from microcontroller) to indicate when to present another bit of data

// error and status reporting LED's, comment out to disable the illumination a given LED
#define STATUS_LED 12 // blue
#define ERROR_LED 13 // red - unlucky for some...
#define READING_LED 14 // green

/* Set the various timers and allocate some storage; the goal is to have sufficient to store readings for
 * BROADCAST_RATE_SECONDS seconds, so add a bit just in case the broadcast poll takes longer to trigger than
 * expected, but the logic will roll round anyway to ensure no overrun
 * 
 */
#define BROADCAST_RATE 15 // seconds
#define PROBE_RATE 1 // seconds

#define ROLLING_AVERAGE_COUNT (int)((BROADCAST_RATE / PROBE_RATE) + 2) // cast to int just in case the division isn't a yummy 0 remainder, and adding 2 [seconds] should be all that is needed

double internalRollingAverage[ROLLING_AVERAGE_COUNT] = {};
double celsiusRollingAverage[ROLLING_AVERAGE_COUNT] = {};
int rollingAveragePosition = 0;
bool indexRolledOver = false; // this will allow broadcastReadings() to know if the whole array should be processed or just up to rollingAveragePosition
double minimumInternal = DBL_MAX;
double maximumInternal = DBL_MIN;

// initialise the thermocouple
Adafruit_MAX31855 thermocouple(CLK, CS, DO);

// initialise the UDP object
WiFiUDP udp;

// track any problems reading the temperatures
bool probeReadingError = false;
#ifdef DEBUG
unsigned long probeReadingErrorCount = 0;
#endif

// reading and broadcast timer controls
bool volatile shouldReadProbes = false;
bool volatile shouldBroadcast = false;

// call back functions kept as simple as possible...
os_timer_t probeTimer;
void probeTimerCallback(void *pArg) {
  shouldReadProbes = true;
}

os_timer_t broadcastTimer;
void broadcastTimerCallback(void *pArg) {
  shouldBroadcast = true;
}

// start reading from the first byte (address 0) of the EEPROM
int address = 0;
byte value;

// set things up...
void setup() {

  #ifdef STARTUP_DELAY
    delay(STARTUP_DELAY);
  #endif

  // endeavour to plop as little code onto the board as possible...
  #ifdef ERROR_LED
    pinMode(ERROR_LED, OUTPUT);
  #endif
  #ifdef STATUS_LED
    pinMode(STATUS_LED, OUTPUT);
  #endif
  #ifdef READING_LED
    pinMode(READING_LED, OUTPUT);
  #endif

  // initialise the serial interface and wait for it to be ready...
  Serial.begin(baudRate);
  while (!Serial) {
    delay(50);
  }

  Serial.println(); Serial.print(F("In setup, "));
  #ifdef DEBUG
    #ifdef VERBOSE
      Serial.println(F("verbose debugging is enabled"));
    #else
      Serial.println(F("debugging is enabled"));
    #endif
  #else
    Serial.println(F("debugging is not enabled"));
    Serial.end(); // close down what isn't needed
  #endif

  // apparently the MAX13855 needs time to 'settle down'
  delay(500);

  #ifdef DEBUG
    Serial.print(F("Attempting to connect to "));
    Serial.print(networkSSID);
  #endif

  WiFi.begin(networkSSID, wifiPassword);
  while (WiFi.status() != WL_CONNECTED) {
    #ifdef STATUS_LED
      flashLED(STATUS_LED, 1);
    #endif
    #if defined(DEBUG) && defined(VERBOSE)
      Serial.print(F("."));
      Serial.flush();
    #endif
    delay (500);
  }

  localIPAddress = WiFi.localIP();
  subnetMask = WiFi.subnetMask();
  broadcastAddress = localIPAddress | (~subnetMask); // wonderful simplicity (http://stackoverflow.com/a/777631/259122)

  #ifdef DEBUG
    Serial.print(F(" connected with IP address "));
    Serial.print(localIPAddress);
    Serial.print(F(" netmask "));
    Serial.print(subnetMask);
    Serial.print(F(" using broadcast address "));
    Serial.println(broadcastAddress);
  #endif

  // setup the call backs to trigger (in milliseconds, hence the multiplier)
  os_timer_setfn(&probeTimer, probeTimerCallback, NULL);
  os_timer_arm(&probeTimer, (PROBE_RATE * 1000), true); // 
  os_timer_setfn(&broadcastTimer, broadcastTimerCallback, NULL);
  os_timer_arm(&broadcastTimer, (BROADCAST_RATE * 1000), true); // 

  #ifdef STATUS_LED
    // visually indicate end of setup...
    flashLED(STATUS_LED, 5);
  #endif
}

// nice and simple :)
void loop() {
  #if defined(DEBUG) && defined(VERBOSE)
    Serial.print(F("shouldReadProbes is "));
    Serial.print(shouldReadProbes);
    Serial.print(F(" and shouldBroadcast is "));
    Serial.println(shouldBroadcast);
  #endif

  // prioritise broadcast over reading the probes
  if (shouldBroadcast) {
    shouldBroadcast = false;
    broadcastReadings();
  }
  if (shouldReadProbes) {
    shouldReadProbes = false;
    readProbes();
  }

  // timer handling states that yield() or a delay() should be called, no point in hammering the CPU so we'll delay a bit
  delay(50);
}

void readProbes() {
  
  double internalTemperature = thermocouple.readInternal();
  double temperatureCelsius = thermocouple.readCelsius();

  // if the sensor can't be read correctly, wait for a cycle and try again
  #ifdef ERROR_LED
    digitalWrite(ERROR_LED, LOW); // turn off the LED
  #endif
  if (isnan(internalTemperature) || isnan(temperatureCelsius)) {
    probeReadingError = true;
    #ifdef DEBUG
      probeReadingErrorCount++;
      Serial.println("Something is wrong with thermocouple! Is it grounded?");
    #endif
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

  #ifdef DEBUG
    Serial.print(F("Internal temperature is "));
    Serial.print(internalTemperature);
    Serial.print(F("°C and the probe is reading (linearised) "));
    Serial.print(temperatureCelsius);
    Serial.print(F("°C ("));
    Serial.print(lineariseTemperature(internalTemperature, temperatureCelsius));
    Serial.print(F("°C) and readError is: "));
    Serial.println(thermocouple.readError());
  #endif

  // some basic protection logic just in case the broadcast hasn't happened in a timely manner...
  if (rollingAveragePosition >= ROLLING_AVERAGE_COUNT) {
    #ifdef DEBUG
      Serial.println("ERROR! resetting rolling average index to zero - broadcast can't have happened at the correct time");
    #endif
    rollingAveragePosition = 0;
    indexRolledOver = true;
  }

  // write the temperature data values to the rolling average array
  internalRollingAverage[rollingAveragePosition] = internalTemperature;
  celsiusRollingAverage[rollingAveragePosition] = lineariseTemperature(internalTemperature, temperatureCelsius);

  // update the array offset for the next capture before looping again
  rollingAveragePosition++;

  #if defined(DEBUG) && defined(VERBOSE)
    // -1 to compensate for the ++
    Serial.print("Setting internalRollingAverage[");
    Serial.print(rollingAveragePosition -1);
    Serial.print("]=");
    Serial.println(internalRollingAverage[rollingAveragePosition -1]);
    Serial.print("Setting celsiusRollingAverage[");
    Serial.print(rollingAveragePosition -1);
    Serial.print("]=");
    Serial.println(celsiusRollingAverage[rollingAveragePosition -1]);
  #endif
}

void broadcastReadings() {

  // if there was a problem reading the probes or there is no data to broadcast, alert and return
  if ((rollingAveragePosition == 0 && !indexRolledOver) || probeReadingError) {
    #ifdef DEBUG
      Serial.print(F("Not going to broadcast readings, rollingAveragePosition is "));
      Serial.print(rollingAveragePosition);
      Serial.print(F(" and probeReadingError is "));
      Serial.println(probeReadingError);
    #endif
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
    #if defined(DEBUG) && defined(VERBOSE)
      Serial.printf("Reading celsiusRollingAverage[%d]=", i);
      Serial.println(celsiusRollingAverage[i]);
    #endif
    broadcastTemperatureC += celsiusRollingAverage[i];
  }

  // reset the fields for readProbes()
  rollingAveragePosition = 0;
  indexRolledOver = false;

  broadcastTemperatureC = broadcastTemperatureC / ((double)endOfIndex); // adding 0.0 forces the int to be cast, probably a crappy way of doing it...
  broadcastTemperatureF = (broadcastTemperatureC * (9.0/5.0)) + 32.0;

  String broadcastMessage = formatBroadcastMessage(broadcastTemperatureC, broadcastTemperatureF);
  byte broadcastBytes[broadcastMessage.length() + 1];
  broadcastMessage.getBytes(broadcastBytes, broadcastMessage.length() + 1);
  #if defined(DEBUG) && defined(VERBOSE)
    Serial.print(F("minimumInternal="));
    Serial.println(minimumInternal);
    Serial.print(F("maximumInternal="));
    Serial.println(maximumInternal);
    Serial.printf("Broadcast [%s] to UDP port %d via ", broadcastBytes, udpRemotePort);
    Serial.println(broadcastAddress);
  #endif

  // check the WiFi connection, then send the datagram
  if (WiFi.status() == WL_CONNECTED) {
    #ifdef DEBUG
      Serial.println(F("Broadcasting..."));
    #endif
    if (udp.beginPacket(broadcastAddress, udpRemotePort) == 1) {
      udp.write(broadcastBytes, broadcastMessage.length() + 0);
      if (udp.endPacket() != 1) {
        #ifdef DEBUG
          Serial.print(F("ERROR! UDP broadcast failed!"));
        #endif
        #ifdef ERROR_LED
          flashLED(ERROR_LED, 3);
        #endif
      #ifdef STATUS_LED // oops, this was set to DEBUG, what a muppet...
      } else {
        flashLED(STATUS_LED, 1);
      #endif
      }
    } else {
      #ifdef DEBUG
        Serial.print(F("ERROR! UDP broadcast setup failed!"));
      #endif
      #ifdef ERROR_LED
        flashLED(ERROR_LED, 3);
      #endif
    }
  } else {
    // a problem occurred! need to implement something better to advertise the fact...
    #ifdef DEBUG
      Serial.print(F("ERROR! WiFi status showing as not connected: "));
      Serial.println(WiFi.status());
    #endif
    #ifdef ERROR_LED
      flashLED(ERROR_LED, 5);
    #endif
  }
}

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

/*
 * Trivial function to format the data to broadcast to listners...
 * Implemented this way to make it easier to tweak as desired
 */
String formatBroadcastMessage(double broadcastTemperatureC, double broadcastTemperatureF) {
  // construct a String object that will contain a basic JSON structure
  // could have used a JSON library but this is so fixed it really didn't seem worth the effort
  String debugBroadcast = "";
  #ifdef DEBUG
    byte macAddress[6];
    WiFi.macAddress(macAddress);
    String macAddressString;
    for (int i = 0; i < 6; i++) {
      macAddressString += String(macAddress[i], HEX);
      if (i < 5) macAddressString += ":";
    }
    debugBroadcast = ",\"debugData\":{\"minimumInternal\":" + String(minimumInternal,3) + // *very* peculiar compiler error if the minimumInternal line is formatted exactly like the (perfectly working?!) maximumInternal line?!
                     ",\"maximumInternal\":" + String(maximumInternal,3) +
                     ",\"BROADCAST_RATE\":" + BROADCAST_RATE +
                     ",\"PROBE_RATE\":" + PROBE_RATE +
                     ",\"probeReadingErrorCount\":" + probeReadingErrorCount +
                     ",\"chipId\":\"" + ESP.getChipId() + // likely to be a number, but wrapped in quotes just in case...
                     "\",\"macAddress\":\"" + macAddressString + 
                     "\"}";
  #endif
  return String("{\"celsius\":" + String(broadcastTemperatureC, 2) + ",\"fahrenheit\":" + String(broadcastTemperatureF, 2) + ",\"probeName\":\"" + probeName + "\""+ debugBroadcast + "}");
}

/* 
 * Function pinched directly from the Adafruit forum http://forums.adafruit.com/viewtopic.php?f=19&t=32086 
 * Note: there are corrections applied from the forum posts, and this has been tweaked to remove some
 * checks made elsewhere and take parameters and provide a return value:
 * 
 * - internalTemp is the coldJunctionCelsiusTemperatureReading parameter
 * - rawTemp is the externalCelsiusTemperatureReading parameter
 * - the linearised value is returned, to convert to fahrenheit do a basic (celsius*(9.0/5.0))+32.0 conversion
 * 
 */

double lineariseTemperature(double coldJunctionCelsiusTemperatureReading, double externalCelsiusTemperatureReading) {

  // Initialize variables.
  int i = 0; // Counter for arrays
  double thermocoupleVoltage= 0;
  double internalVoltage = 0;
  double correctedTemperature = 0;

  // Steps 1 & 2. Subtract cold junction temperature from the raw thermocouple temperature.
  thermocoupleVoltage = (externalCelsiusTemperatureReading - coldJunctionCelsiusTemperatureReading)*0.041276; // C * mv/C = mV
//  #if defined(DEBUG) && defined(VERBOSE)
//    Serial.println();
//    Serial.print(F("Thermocouple Voltage calculated as "));
//    Serial.println(String(thermocoupleVoltage, 5));
//  #endif

  // Step 3. Calculate the cold junction equivalent thermocouple voltage.
  if (coldJunctionCelsiusTemperatureReading >= 0) { // For positive temperatures use appropriate NIST coefficients
    // Coefficients and equations available from http://srdata.nist.gov/its90/download/type_k.tab
    double c[] = {-0.176004136860E-01,  0.389212049750E-01,  0.185587700320E-04, -0.994575928740E-07,  0.318409457190E-09, -0.560728448890E-12,  0.560750590590E-15, -0.320207200030E-18,  0.971511471520E-22, -0.121047212750E-25};

    // Count the the number of coefficients. There are 10 coefficients for positive temperatures (plus three exponential coefficients),
    // but there are 11 coefficients for negative temperatures.
    int cLength = sizeof(c) / sizeof(c[0]);
    
    // Exponential coefficients. Only used for positive temperatures.
    double a0 =  0.118597600000E+00;
    double a1 = -0.118343200000E-03;
    double a2 =  0.126968600000E+03;
    
    // From NIST: E = sum(i=0 to n) c_i t^i + a0 exp(a1 (t - a2)^2), where E is the thermocouple voltage in mV and t is the temperature in degrees C.
    // In this case, E is the cold junction equivalent thermocouple voltage.
    // Alternative form: C0 + C1*internalTemp + C2*internalTemp^2 + C3*internalTemp^3 + ... + C10*internaltemp^10 + A0*e^(A1*(internalTemp - A2)^2)
    // This loop sums up the c_i t^i components.
    for (i = 0; i < cLength; i++) {
      internalVoltage += c[i] * pow(coldJunctionCelsiusTemperatureReading, i);
    }

    // This section adds the a0 exp(a1 (t - a2)^2) components.
    internalVoltage += a0 * exp(a1 * pow((coldJunctionCelsiusTemperatureReading - a2), 2));

  } else if (coldJunctionCelsiusTemperatureReading < 0) {
    // for negative temperatures
    double c[] = {0.000000000000E+00,  0.394501280250E-01,  0.236223735980E-04, -0.328589067840E-06, -0.499048287770E-08, -0.675090591730E-10, -0.574103274280E-12, -0.310888728940E-14, -0.104516093650E-16, -0.198892668780E-19, -0.163226974860E-22};

    // Count the number of coefficients.
    int cLength = sizeof(c) / sizeof(c[0]);
    
    // Below 0 degrees Celsius, the NIST formula is simpler and has no exponential components: E = sum(i=0 to n) c_i t^i
    for (i = 0; i < cLength; i++) {
      internalVoltage += c[i] * pow(coldJunctionCelsiusTemperatureReading, i) ;
    }
  }
//  #if defined(DEBUG) && defined(VERBOSE)
//    Serial.print(F("Internal Voltage calculated as "));
//    Serial.println(String(internalVoltage, 5));
//  #endif

  // Step 4. Add the cold junction equivalent thermocouple voltage calculated in step 3 to the thermocouple voltage calculated in step 2.
  double totalVoltage = thermocoupleVoltage + internalVoltage;
  
  // Step 5. Use the result of step 4 and the NIST voltage-to-temperature (inverse) coefficients to calculate the cold junction compensated, linearized temperature value.
  // The equation is in the form correctedTemp = d_0 + d_1*E + d_2*E^2 + ... + d_n*E^n, where E is the totalVoltage in mV and correctedTemp is in degrees C.
  // NIST uses different coefficients for different temperature subranges: (-200 to 0C), (0 to 500C) and (500 to 1372C).
  if (totalVoltage < 0) { // Temperature is between -200 and 0C.
     double d[] = {0.0000000E+00, 2.5173462E+01, -1.1662878E+00, -1.0833638E+00, -8.9773540E-01, -3.7342377E-01, -8.6632643E-02, -1.0450598E-02, -5.1920577E-04, 0.0000000E+00};
  
     int dLength = sizeof(d) / sizeof(d[0]);
     for (i = 0; i < dLength; i++) {
        correctedTemperature += d[i] * pow(totalVoltage, i);
     }
  }
  else if (totalVoltage < 20.644) {
    // Temperature is between 0C and 500C.
    double d[] = {0.000000E+00, 2.508355E+01, 7.860106E-02, -2.503131E-01, 8.315270E-02, -1.228034E-02, 9.804036E-04, -4.413030E-05, 1.057734E-06, -1.052755E-08};
    int dLength = sizeof(d) / sizeof(d[0]);
    for (i = 0; i < dLength; i++) {
      correctedTemperature += d[i] * pow(totalVoltage, i);
    }
  }
  else if (totalVoltage < 54.886 ) {
    // Temperature is between 500C and 1372C.
    double d[] = {-1.318058E+02, 4.830222E+01, -1.646031E+00, 5.464731E-02, -9.650715E-04, 8.802193E-06, -3.110810E-08, 0.000000E+00, 0.000000E+00, 0.000000E+00};
    int dLength = sizeof(d) / sizeof(d[0]);
    for (i = 0; i < dLength; i++) {
      correctedTemperature += d[i] * pow(totalVoltage, i);
    }
  } else {
    // NIST only has data for K-type thermocouples from -200C to +1372C. If the temperature is not in that range, set temp to impossible value.
    #ifdef DEBUG
      Serial.println("Temperature is out of range, this should never happen!");
    #endif
    #ifdef ERROR_LED
      flashLED(ERROR_LED, 2);
    #endif
    correctedTemperature = NAN;
  }

//  #if defined(DEBUG) && defined(VERBOSE)
//    Serial.print("Corrected temperature calculated to be: ");
//    Serial.print(correctedTemperature, 5);
//    Serial.println("°C");
//    Serial.println();
//  #endif
  return correctedTemperature;
}

/*************************************************** 
  This is an example for the Adafruit Thermocouple Sensor w/MAX31855K

  Designed specifically to work with the Adafruit Thermocouple Sensor
  ----> https://www.adafruit.com/products/269

  These displays use SPI to communicate, 3 pins are required to  
  interface
  Adafruit invests time and resources providing this open source code, 
  please support Adafruit and open-source hardware by purchasing 
  products from Adafruit!

  Written by Limor Fried/Ladyada for Adafruit Industries.  
  BSD license, all text above must be included in any redistribution
 ****************************************************/
