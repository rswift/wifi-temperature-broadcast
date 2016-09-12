/*
   This is a simple application, firmly inspired by Roasthacker to capture and broadcast
   the bean mass temperature from my Gene Café CBR-101 coffee roaster. The ultimate goal
   is to feed these temperatures into the Roastmaster Data Logger for hands-free temperature
   capture directly on the iPad...

   It has been written and tested on the Adafruit HUZZAH ESP8266 with the Adafruit MAX31855
   plus K-Type Thermocouple. It is therefore based on the many, excellent example sketches
   available within the Arduino IDE and elsewhere...

   There are two payloads, both formatted as JSON, one adheres to the Roastmaster Datagram
   Protocol, the other is my own concoction...

   This stuff really is amazing, when I think how big the 300 baud modems I used to provide
   remote support for customers around the globe, to be able to get this much capability
   into a matchbox is phenomenal :)

   I've decided to adopt the MIT licence for this project, I'd originally included the licence from
   the starting Adafruit example (serialthermocouple.ino), so that is retained in this source code,
   however, the text didn't sit right so I've clarified it. None of that alters the fact that
   credit must go to the Adafruit team for their efforts and examples.

   Danny from Roastmaster provides an example application on his Github page, and as of August 2016
   his software is still at the pre-release stage so is potentially subject to change. Also note
   that he is releasing his software under the MIT Licence.

   Usage:
    - set the probeName, WiFi and UDP broadcast port settings
    - ensure a UDP trigger is active, such as HallEffectBroadcast.ino
    - ensure the MAX31855 pins are assigned correctly
    - tinker with the poll rates, debug/verbose and baud rate values as you prefer
    - flash the ESP8266
    - monitor your network for UDP datagrams sent to the port specified by udpRemotePort
    - and/or consume the datagram in a suitable application

   Some links:
    - Roastmaster Datagram Protocol: https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs
    - Hall Effect based trigger: https://github.com/rswift/wifi-temperature-broadcast/wiki/External-Trigger-(Hall-Effect-Sensor)

    - Roasthacker: http://roasthacker.com/?p=529 & http://roasthacker.com/?p=552
    - Adafruit ESP8266: https://www.adafruit.com/products/2471
    - Adafruit MAX31855: https://www.adafruit.com/products/269 & https://cdn-shop.adafruit.com/datasheets/MAX31855.pdf
    - Using a thermocouple: https://learn.adafruit.com/thermocouple/using-a-thermocouple
    - Calibration: https://learn.adafruit.com/calibrating-sensors/maxim-31855-linearization
    - TCP dump or Wireshark for packet capture: http://www.tcpdump.org/tcpdump_man.html or https://www.wireshark.org/

    Robert Swift - September 2016.
*/

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <float.h> // provides min/max double values
#include "Adafruit_MAX31855.h"
#include <ArduinoJson.h>

// disabled by default, but can be overridden via EEPROM and in the future, via the web server...
bool debugLogging = false;
bool verboseLogging = false;

// put this up near the top, to make it easy to spot for those not using the same baud rate as me for their FTDI Friend...
const unsigned int baudRate = 115200;

// comment out to remove all startup delay, I found it was necessary to let the board get itself together
const int startupDelay = 1500;

// Set the WiFi and network details
IPAddress localIPAddress, rdpIPAddress, subnetMask, multicastAddress(239, 31, 8, 55), rdpMulticastAddress(224, 0, 0, 1), cncMulticastAddress(239, 9, 80, 1), broadcastAddress;
word udpRemotePort = 31855; // for my own needs
word rdpRemotePort = 5050;  // for Roastmaster
word cncPort = 9801;

// a check for EEPROM health and a simple struct to make reading/writing easier
const byte eepromComparison = B10101010;
struct eeprom_config {
  byte healthBight = !eepromComparison;
  char networkSSID[WL_SSID_MAX_LENGTH] = "SSID";
  char wifiPassword[WL_WPA_KEY_MAX_LENGTH] = "PASSWORD";
  bool debugEnabled = true;
  bool verboseEnabled = false;
};

// error and status reporting LED's, comment out to disable the illumination a given LED
const int statusLed = 12; // blue
const int errorLed = 13; // red - unlucky for some...
const int readingLed = 14; // green

/* Set the various timers and allocate some storage; the goal is to have sufficient to store readings for
   BROADCAST_RATE_SECONDS seconds, so add a bit just in case the broadcast poll takes longer to trigger than
   expected, but the logic will roll round anyway to ensure no overrun
*/
#define DRUM_ROTATION_SPEED 8  // seconds
#define PROBE_RATE 0.25        // seconds
#define ROLLING_AVERAGE_COUNT (int)(((DRUM_ROTATION_SPEED / 2) / PROBE_RATE) * 1.25) // cast to int just in case the division isn't a yummy 0 remainder, and add 25% as a contingency

double celsiusRollingAverage[ROLLING_AVERAGE_COUNT] = {};
int rollingAveragePosition = 0;
bool indexRolledOver = false; // this will allow broadcastReadings() to know if the whole array should be processed or just up to rollingAveragePosition
double minimumInternal = DBL_MAX;
double maximumInternal = DBL_MIN;

// Define the pin mapping from the MAX31855 interface to the microcontroller (https://learn.adafruit.com/thermocouple/using-a-thermocouple)
#define DO 2  // DO (data out) is an output from the MAX31855 (input to the microcontroller) which carries each bit of data
#define CS 4  // CS (chip select) is an input to the MAX31855 (output from the microcontroller) and tells the chip when its time to read the thermocouple and output more data
#define CLK 5 // CLK (clock) is an input to the MAX31855 (output from microcontroller) to indicate when to present another bit of data

Adafruit_MAX31855 thermocouple(CLK, CS, DO);

// initialise the UDP objects (command & control, broadcast and Roastmaster)
WiFiUDP brdUdp;
WiFiUDP rdpUdp;
WiFiUDP cncUdp;

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

// set the reference for battery monitoring
ADC_MODE(ADC_VCC);

/* Taken directly from https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs/blob/master/Roastmaster_RDP_Probe_Host_SBC.ino
   and https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs/blob/master/RDP%20Data%20Sheet.pdf
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
    Lots of things happen during the setup, but mainly, WiFi connectivity then handshake with Roastmaster
*/
void setup() {

  // give the ESP8266 a moment to calm down...
  delay(startupDelay);

  // endeavour to plop as little code onto the board as possible...
  pinMode(errorLed, OUTPUT);
  pinMode(statusLed, OUTPUT);
  pinMode(readingLed, OUTPUT);

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
    if (verboseLogging) { Serial.print(F(" using password ")); Serial.print(config.wifiPassword); }
  }

  // station mode then attempt connections until granted...
  WiFi.mode(WIFI_STA);
  long connectDelay = 10;
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(config.networkSSID, config.wifiPassword);
    flashLED(statusLed, 1);
    if (debugLogging && verboseLogging) {
      Serial.print(F(".")); Serial.flush();
    }
    delay(connectDelay);
    connectDelay += connectDelay;
  }

  localIPAddress = WiFi.localIP();
  subnetMask = WiFi.subnetMask();
  broadcastAddress = localIPAddress | (~subnetMask); // wonderful simplicity (http://stackoverflow.com/a/777631/259122)

  if (debugLogging) { Serial.print(F(" connected with IP address ")); Serial.print(localIPAddress); Serial.print(F(" netmask ")); Serial.print(subnetMask); Serial.print(F(" using broadcast address ")); Serial.println(broadcastAddress); }

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
          if (debugLogging) { Serial.print(F("Received ACK from Roastmaster, will broadcast to ")); Serial.print(rdpIPAddress);Serial.print(F(":")); Serial.println(rdpRemotePort); }
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
  if (!gotRDPServer && debugLogging) { Serial.print(F("No RDP Server found...")); }

  // configure the Command & Control UDP listener
  cncUdp.beginMulticast(WiFi.localIP(), cncMulticastAddress, cncPort);

  // signal end of startup
  digitalWrite(errorLed, HIGH);
  digitalWrite(statusLed, HIGH);
  delay(357);
  digitalWrite(statusLed, LOW);
  flashLED(statusLed, 2);
  delay(123);
  digitalWrite(errorLed, LOW);
}

/*
    The main loop is loosely based on the tilt switch hardware interrupt, except this version checks for an incoming UDP packet
    as the trigger for the same thing. This will be expanded at some point to allow things like debug/verbose logging to be remotely
    activated etc. but the goal is to keep it as slim as possible...
*/
void loop() {
  if (debugLogging) { Serial.print(F("shouldReadProbes is ")); Serial.print(shouldReadProbes); Serial.print(F(", shouldBroadcast is ")); Serial.print(shouldBroadcast); Serial.print(F(" and haveBroadcastSinceRead is ")); Serial.println(haveBroadcastSinceRead); }

  // check for a command and control message
  checkForCommandAndControlMessage();

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

/***************************************************
  This is built on the Adafruit Thermocouple Sensor w/MAX31855K example

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
