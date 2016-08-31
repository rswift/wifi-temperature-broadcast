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
 * There are two payloads, both formatted as JSON, one adheres to the Roastmaster Datagram
 * Protocol, the other is my own concoction...
 * 
 * This stuff really is amazing, when I think how big the 300 baud modems I used to provide
 * remote support for customers around the globe, to be able to get this much capability
 * into a matchbox is phenomenal :)
 * 
 * This code is issued under the "Bill and Ted - be excellent to each other" licence (which
 * has no content, just, well, be excellent to each other), however, I have included the licence
 * files from my original starting file (serialthermocouple.ino) at the bottom of this sketch...
 * 
 * Danny from Roastmaster provides an example application on his Github page, and as of August 2016
 * his software is still at the pre-release stage so is potentially subject to change. Also note
 * that he is releasing his software under the MIT Licence.
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
 *  - remove hardware tilt switch trigger as this is not sufficiently accurate, replace
 *    with hall effect switch and a WiFi triggered command
 *  - build a small server element into the code to receive instructions (broadcast now,
 *    configuration etc.)
 *  - test on other boards
 *  - simplify the code, maybe separate out although it is handy having it all in a
 *    single file (to a point!)
 * 
 * Some links:
 *  - Roastmaster Datagram Protocol: https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs
 *  
 *  - Roasthacker: http://roasthacker.com/?p=529 & http://roasthacker.com/?p=552
 *  - Adafruit ESP8266: https://www.adafruit.com/products/2471
 *  - Adafruit MAX31855: https://www.adafruit.com/products/269 & https://cdn-shop.adafruit.com/datasheets/MAX31855.pdf
 *  - Using a thermocouple: https://learn.adafruit.com/thermocouple/using-a-thermocouple
 *  - Calibration: https://learn.adafruit.com/calibrating-sensors/maxim-31855-linearization
 *  - TCP dump or Wireshark for packet capture: http://www.tcpdump.org/tcpdump_man.html or https://www.wireshark.org/
 *  - Timer: http://www.switchdoc.com/2015/10/iot-esp8266-timer-tutorial-arduino-ide/
 *  
 *  Robert Swift - August 2016.
 */

#include <Arduino.h>
#include <EEPROM.h>
#include <float.h> // provides min/max double values

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

#include "Adafruit_MAX31855.h"
#include <ArduinoJson.h>

extern "C" {
#include "user_interface.h" // needed for the timer
}

// disabled by default, but can be overridden via EEPROM and in the future, via the web server...
bool debugLogging = false;
bool verboseLogging = false;

// put this up near the top, to make it easy to spot for those not using the same baud rate as me for their FTDI Friend...
const unsigned int baudRate = 115200;

// comment out to remove all startup delay, I found it was necessary to let the board get itself together
#define STARTUP_DELAY 1500

// Set the WiFi and network details
IPAddress localIPAddress, rdpIPAddress, subnetMask, multicastAddress(239,31,8,55), rdpMulticastAddress(224,0,0,1), broadcastAddress;
word udpRemotePort = 31855; // for my own needs
word rdpRemotePort = 5050;  // for Roastmaster

// a check for EEPROM health and a simple struct to make reading/writing easier
const byte eepromComparison = B10101010;

struct eeprom_config {
  byte healthBight = !eepromComparison;
  char networkSSID[WL_SSID_MAX_LENGTH] = "SSID";
  char wifiPassword[WL_WPA_KEY_MAX_LENGTH] = "PASSWORD";
  bool debugEnabled = true;
  bool verboseEnabled = true;
};

// Define the pin mapping from the MAX31855 interface to the microcontroller (https://learn.adafruit.com/thermocouple/using-a-thermocouple)
#define DO 2  // DO (data out) is an output from the MAX31855 (input to the microcontroller) which carries each bit of data
#define CS 4  // CS (chip select) is an input to the MAX31855 (output from the microcontroller) and tells the chip when its time to read the thermocouple and output more data
#define CLK 5 // CLK (clock) is an input to the MAX31855 (output from microcontroller) to indicate when to present another bit of data

// error and status reporting LED's, comment out to disable the illumination a given LED
#define STATUS_LED 12  // blue
#define ERROR_LED 13   // red - unlucky for some...
#define READING_LED 14 // green

// the pin to interrupt on
#define SWITCH_PIN 0 // probably a bad choice, but I'm a bit stuck unless I de-sugru the whole thing and start again...

/* Set the various timers and allocate some storage; the goal is to have sufficient to store readings for
 * BROADCAST_RATE_SECONDS seconds, so add a bit just in case the broadcast poll takes longer to trigger than
 * expected, but the logic will roll round anyway to ensure no overrun
 */
#define DRUM_ROTATION_SPEED 8  // seconds
#define PROBE_RATE 0.25        // seconds
#define ROLLING_AVERAGE_COUNT (int)(((DRUM_ROTATION_SPEED / 2) / PROBE_RATE) * 1.25) // cast to int just in case the division isn't a yummy 0 remainder, and add 25% as a contingency

double celsiusRollingAverage[ROLLING_AVERAGE_COUNT] = {};
int rollingAveragePosition = 0;
bool indexRolledOver = false; // this will allow broadcastReadings() to know if the whole array should be processed or just up to rollingAveragePosition
double minimumInternal = DBL_MAX;
double maximumInternal = DBL_MIN;

// initialise the thermocouple
Adafruit_MAX31855 thermocouple(CLK, CS, DO);

// initialise the UDP objects, one for command & control, the other for Roastmaster
WiFiUDP udp;
WiFiUDP rdpUdp;

// track any problems reading the temperatures
bool probeReadingError = false;
unsigned long probeReadingErrorCount = 0;

// https://github.com/rswift/wifi-temperature-broadcast/wiki/JSON-Payload-Format
const int probeType = 0; // TEMPERATURE
const int temperatureProbeType = 0; // BEAN_MASS
const char probeName[] = "Gene Café Bean Mass";

int rdpEpoch = 1; // all reference will be rdpEpoch++ to keep the value going up, chances of rolling over are square root of not much...
const int rdpMaxAckListenAttempts = 10; // arbitary value based on no science whatsoever...
bool gotRDPServer = false;

// reading and broadcast timer controls
volatile bool shouldReadProbes = false;
volatile bool shouldBroadcast = false;
volatile bool haveBroadcastSinceRead = false;
volatile unsigned long detachedAt = 0;          // essentially a debouncer, need to ensure that at least a given period of time has passed before setting a new interrupt
const unsigned long reattachmentDelay = 2000;  // set at two seconds, drum takes about seven to rotate fully so this should be enough time to allow the switch to settle

// set the reference for battery monitoring
ADC_MODE(ADC_VCC);

/* Taken directly from https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs/blob/master/Roastmaster_RDP_Probe_Host_SBC.ino 
 * and https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs/blob/master/RDP%20Data%20Sheet.pdf
 */
// RDP Keys
#define RDPKey_Version "RPVersion"
#define RDPKey_Serial "RPSerial"
#define RDPKey_Epoch "RPEpoch"
#define RDPKey_Payload "RPPayload"
#define RDPKey_EventType "RPEventType"
#define RDPKey_Channel "RPChannel"
#define RDPKey_Value "RPValue"
#define RDPKey_Meta "RPMetaType"

// RDP String Constants
#define RDPValue_Version "RDP_1.0"

// RDP Event Type Integer Constants
typedef enum {
    RDPEventType_SYN = 1,
    RDPEventType_ACK,
    RDPEventType_Temperature,
    RDPEventType_Control,
    RDPEventType_Pressure,
    RDPEventType_Remote,
} RDPEventType;

// RDP Meta Type Integer Constants
typedef enum {

    //Temperature meta constants
    //Valid with event type RDPEventType_Temperature
    RDPMetaType_BTTemp = 1000,
    RDPMetaType_ETTemp,
    RDPMetaType_METTemp,
    RDPMetaType_HeatBoxTemp,
    RDPMetaType_ExhaustTemp,
    RDPMetaType_AmbientTemp,
    RDPMetaType_BTCoolingTemp,
} RDPMetaType;

/* 
 *  Lots of things happen during the setup, but mainly, WiFi connectivity then handshake with Roastmaster
 */
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

  // read the EEPROM data and check the first byte for good health
  eeprom_config config;
  const int eepromAddress = 0;
  EEPROM.begin(sizeof(config.healthBight));
  EEPROM.get(eepromAddress, config.healthBight);

  if (debugLogging && verboseLogging) {
    Serial.print("EEPROM config size: "); Serial.println(sizeof(config));
    Serial.print("EEPROM config.healthBight: "); Serial.println(config.healthBight, BIN);
    Serial.print("EEPROM health comparison : "); Serial.println(eepromComparison, BIN);
    Serial.print("EEPROM config.networkSSID: "); Serial.print(config.networkSSID); Serial.println(F("¶")); // pilcrow to make it clear where the end of the value is...
    Serial.print("EEPROM config.wifiPassword: "); Serial.print(config.wifiPassword); Serial.println(F("¶"));
}

  if (config.healthBight == eepromComparison) {
    // health check passes, read the whole thing...
    EEPROM.begin(sizeof(config));
    EEPROM.get(eepromAddress, config);

  } else {
    // the comparison doesn't match, so write the data
    if (debugLogging) { Serial.println(F("Writing data to EEPROM as health byte didn't match...")); }

    config.healthBight = eepromComparison;
    EEPROM.begin(sizeof(config));
    EEPROM.put(eepromAddress, config);
    EEPROM.commit();
  }

  // transfer logging settings
  debugLogging = config.debugEnabled;
  verboseLogging = config.verboseEnabled;

  Serial.println(); Serial.printf("In setup, %sdebugging is%senabled\n", ((verboseLogging) ? "verbose " : ""), ((debugLogging) ? " " : " not "));
  if (!debugLogging && !verboseLogging) {
    Serial.end(); // close down what isn't needed
  }

  if (debugLogging) {
    Serial.print(F("Attempting to connect to ")); Serial.print(config.networkSSID);
    if (verboseLogging) {
      Serial.print(F(" using password ")); Serial.print(config.wifiPassword);
    }
  }

  // station mode then attempt connections until granted...
  WiFi.mode(WIFI_STA);
  long connectDelay = 10;
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(config.networkSSID, config.wifiPassword);
    #ifdef STATUS_LED
      flashLED(STATUS_LED, 1);
    #endif
    if (debugLogging && verboseLogging) { Serial.print(F(".")); Serial.flush(); }
    delay(connectDelay);
    connectDelay += connectDelay;
  }

  localIPAddress = WiFi.localIP();
  subnetMask = WiFi.subnetMask();
  broadcastAddress = localIPAddress | (~subnetMask); // wonderful simplicity (http://stackoverflow.com/a/777631/259122)

  if (debugLogging) { Serial.print(F(" connected with IP address ")); Serial.print(localIPAddress); Serial.print(F(" netmask ")); Serial.print(subnetMask); Serial.print(F(" using broadcast address ")); Serial.println(broadcastAddress); }

  // setup the switch interrupt
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_PIN), switchActivated, RISING);

  // at this point we have established a WiFi connection and initialised everything needed, so connect to Roastmaster and wait until the SYN/ACK has been completed...
  DynamicJsonBuffer jsonSYNBuffer;
  JsonObject& rdpHandshakeTransmission = jsonSYNBuffer.createObject();
  rdpHandshakeTransmission[RDPKey_Version] = RDPValue_Version;
  rdpHandshakeTransmission[RDPKey_Serial] = probeName;
  rdpHandshakeTransmission[RDPKey_Epoch] = rdpEpoch++;
  JsonArray& rdpPayload = rdpHandshakeTransmission.createNestedArray(RDPKey_Payload);
  JsonObject& rdpSYN = rdpPayload.createNestedObject();
  rdpSYN[RDPKey_EventType] = (int)RDPEventType_SYN;

  int synTransmissionLength = rdpHandshakeTransmission.measureLength();
  String synTransmissionString;
  byte synTransmissionBytes[synTransmissionLength + 1];
  rdpHandshakeTransmission.printTo(synTransmissionString);
  synTransmissionString.getBytes(synTransmissionBytes, synTransmissionLength + 1);

  if (debugLogging) { Serial.print(F("RDP SYN object (")); Serial.print(synTransmissionLength); Serial.print(F(" bytes): ")); rdpHandshakeTransmission.prettyPrintTo(Serial); Serial.println(); }

  // start listening before transmission as there is a good chance the iPad will be much faster than the ESP8266...
  rdpUdp.begin(rdpRemotePort);
  rdpUdp.beginPacketMulticast(rdpMulticastAddress, rdpRemotePort, localIPAddress);
  rdpUdp.write(synTransmissionBytes, synTransmissionLength);
  rdpUdp.endPacket();

  // loop, reusing connectDelay (for rdpMaxAckListenAttempts times) until an ACK has been received from Roastmaster
  connectDelay = 5;
  int rdpAckRetries = 0;
  while (rdpAckRetries <= rdpMaxAckListenAttempts) {
    int packetSize = rdpUdp.parsePacket();
    if (packetSize) {
      // we have data, confirm the ACK
      rdpIPAddress = rdpUdp.remoteIP();
      char rdpAckPacketBuffer[packetSize];
      rdpUdp.read(rdpAckPacketBuffer, packetSize);

      DynamicJsonBuffer jsonACKBuffer;
      JsonObject& rdpAckJsonResponse = jsonACKBuffer.parseObject(rdpAckPacketBuffer);
      if (rdpAckJsonResponse.success()) {
        if (debugLogging) { Serial.print(F("Received datagram from ")); Serial.print(rdpIPAddress); Serial.print(F(" [")); rdpAckJsonResponse.printTo(Serial); Serial.print(F("] length=")); Serial.println(packetSize); }

        // extract the event type (an array) which must match RDPEventType_ACK to be valid
        int rdpAckEventType = rdpAckJsonResponse[RDPKey_Payload][0][RDPKey_EventType].as<unsigned int>();
        if (rdpAckEventType == (int)RDPEventType_ACK) {
          gotRDPServer = true;
          if (debugLogging) { Serial.print(F("Received ACK from Roastmaster, will broadcast to ")); Serial.print(rdpIPAddress); Serial.print(F(":")); Serial.println(rdpRemotePort); }
        } else {
          if (debugLogging) { Serial.print(F("Datagram is valid JSON, but does not match a valid Roastmaster ACK response. Will therefore not broadcast to ")); Serial.println(rdpIPAddress); }
        }
      } else {
        if (debugLogging) { Serial.print(F("Failed to parse UDP datagram as valid JSON! Response: ")); Serial.println(rdpAckPacketBuffer); }
      }
      break; // all done :)

    } else {
      if (debugLogging && verboseLogging) { Serial.print(F("No ACK received, pausing for delay: ")); Serial.println(connectDelay); }
      delay(connectDelay);
      connectDelay += connectDelay;
      rdpAckRetries++;
    }
  }

  #ifdef STATUS_LED
    flashLED(STATUS_LED, 5); // visually indicate end of setup...
  #endif

}

// nice and simple :)
void loop() {
  if (debugLogging && verboseLogging) { Serial.print(F("shouldReadProbes is ")); Serial.print(shouldReadProbes); Serial.print(F(", shouldBroadcast is ")); Serial.print(shouldBroadcast); Serial.print(F(" and haveBroadcastSinceRead is ")); Serial.println(haveBroadcastSinceRead); }

  if ((millis() - detachedAt) > reattachmentDelay) {
    if (shouldReadProbes) {
      attachInterrupt(digitalPinToInterrupt(SWITCH_PIN), switchDeactivated, FALLING);
    } else {
      attachInterrupt(digitalPinToInterrupt(SWITCH_PIN), switchActivated, RISING);
    }
  }

  // prioritise broadcast over reading the probes
  if (shouldBroadcast && !haveBroadcastSinceRead) {
    broadcastReadings();
    shouldBroadcast = false;
    shouldReadProbes = false;
    haveBroadcastSinceRead = true;
  }
  if (shouldReadProbes) {
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
    if (udp.beginPacket(broadcastAddress, udpRemotePort) == 1 && (gotRDPServer && rdpUdp.beginPacket(rdpIPAddress, rdpRemotePort) == 1)) {
//    if (udp.beginPacketMulticast(multicastAddress, udpRemotePort, WiFi.localIP()) == 1) {
      udp.write(broadcastBytes, broadcastMessage.length() + 0);
      rdpUdp.write(roastmasterBroadcastBytes, roastmasterMessage.length() + 0);

      byte udpEndPacket = udp.endPacket();
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

// Trivial function to format the data to broadcast to listners...
String formatBroadcastMessage(double broadcastTemperatureC) {
  // possibly slower, but the overhead isn't a major concern in relation to the benefit
  // https://github.com/bblanchon/ArduinoJson/wiki/FAQ#what-are-the-differences-between-staticjsonbuffer-and-dynamicjsonbuffer
  DynamicJsonBuffer jsonBuffer;
  JsonObject& jsonResponse = jsonBuffer.createObject();

  JsonArray& readings = jsonResponse.createNestedArray("readings");
  JsonObject& reading = readings.createNestedObject();
  reading["reading"] = double_with_n_digits(broadcastTemperatureC, 2);
  reading["scale"] = "C";
  reading["probeName"] = probeName;
  reading["probeType"] = probeType;
  reading["probeSubType"] = temperatureProbeType;

  JsonObject& systemInformation = jsonResponse.createNestedObject("systemInformation");
  systemInformation["VCC"] = double_with_n_digits((ESP.getVcc() / 1024.0), 3);

  if (debugLogging) {
    JsonObject& debugData = jsonResponse.createNestedObject("debugData");
    debugData["minimumInternal"] = double_with_n_digits(minimumInternal, 3);
    debugData["maximumInternal"] = double_with_n_digits(maximumInternal, 3);
    debugData["indexRolledOver"] = indexRolledOver;
    debugData["rollingAveragePosition"] = rollingAveragePosition;
    int celsiusRollingAverageSize = sizeof(celsiusRollingAverage) / sizeof(celsiusRollingAverage[0]);
    debugData["celsiusRollingAverageSize"] = celsiusRollingAverageSize;
    JsonArray& rawCelsiusReadingValues = debugData.createNestedArray("rawCelsiusReadingValues");
    for (int i = 0; i < celsiusRollingAverageSize; i++) {
      rawCelsiusReadingValues.add(celsiusRollingAverage[i]);
    }
    debugData["DRUM_ROTATION_SPEED"] = DRUM_ROTATION_SPEED;
    debugData["PROBE_RATE"] = PROBE_RATE;
    debugData["probeReadingErrorCount"] = probeReadingErrorCount;
    debugData["chipId"] = ESP.getChipId();
    Serial.print(F("JSON Object: ")); jsonResponse.prettyPrintTo(Serial); Serial.println();
  }

  String broadcastData;
  jsonResponse.printTo(broadcastData);
  return broadcastData;
}

// see https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs
String formatRoastmasterDatagramProtocolMessage(double broadcastTemperatureC) {
  DynamicJsonBuffer jsonTemperatureBuffer;
  JsonObject& rdpTemperatureTransmission = jsonTemperatureBuffer.createObject();
  rdpTemperatureTransmission[RDPKey_Version] = RDPValue_Version;
  rdpTemperatureTransmission[RDPKey_Serial] = probeName;
  rdpTemperatureTransmission[RDPKey_Epoch] = rdpEpoch++;
  JsonArray& rdpPayload = rdpTemperatureTransmission.createNestedArray(RDPKey_Payload);
  JsonObject& rdpReading = rdpPayload.createNestedObject();
  rdpReading[RDPKey_Channel] = 1;
  rdpReading[RDPKey_EventType] = (int)RDPEventType_Temperature;
  rdpReading[RDPKey_Value] = double_with_n_digits(broadcastTemperatureC, 2);
  rdpReading[RDPKey_Meta] = (int)RDPMetaType_BTTemp;

  if (debugLogging) {
    Serial.print(F("JSON Object: "));
    rdpTemperatureTransmission.prettyPrintTo(Serial);
    Serial.println();
  }

  String broadcastData;
  rdpTemperatureTransmission.printTo(broadcastData);
  return broadcastData;
}
/* interrupt handler, to be kept to the bare minimum
   the logic is simple, if the switch is 'on' then read the probes, when it goes off, broadcast the readings
 */
void switchActivated() {
  // switch has gone on, start reading and do not broadcast
  shouldReadProbes = true;
  shouldBroadcast = false;
  haveBroadcastSinceRead = false;
  detachInterrupt(digitalPinToInterrupt(SWITCH_PIN));
  detachedAt = millis();
}

void switchDeactivated() {
  shouldReadProbes = false;
  shouldBroadcast = true;
  detachInterrupt(digitalPinToInterrupt(SWITCH_PIN));
  detachedAt = millis();
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
    if (debugLogging && verboseLogging) {
      Serial.println("Temperature is out of range, this should never happen!");
    }
    #ifdef ERROR_LED
      flashLED(ERROR_LED, 2);
    #endif
    correctedTemperature = NAN;
  }

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
