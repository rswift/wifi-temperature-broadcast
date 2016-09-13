/*
 *  WiFi UDP client to transmit simple control packet when a switch is triggered to
 *  allow much greater control over when the probes in the roasting bean mass are
 *  actually being read (the tilt switch worked but was easily knocked, meaning the
 *  timing of the start/end of readings was unrealiable, leading to supurious values
 *  being reported)... With this approach, I can move the magnets to start (south pole)
 *  then stop reading very precisely :)
 *  
 *  The logic is simple, the hardware switching on/off triggers an interrupt, which
 *  sets a boolean true/false. The main loop transmits a start/stop message via UDP
 *  based on the value of the boolean.
 *  
 *  The hardware switch is linked to a latching hall effect sensor switch, activated
 *  and deactivated by the different poles on a magnet which is attached to the rotating
 *  roasting drum. South pole activation for my Honeywell SS460S.
 *  
 *  The EEPROM and WiFi code is largely lifted straight from the WiFi Temperature
 *  Broadcast code...
 *
 *  One day Roastmaster may be able to allow me to broadcast ambient temperature and
 *  humidity values directly to it, hence adding in the sensor here while I was wiring
 *  up the 
 *  
 *  ToDo:
 *   - add ambient temperature & humidity from Adafruit SHT31 broadcaster (currently reading only)
 *   - consider 'manual override' switch to allow me to force probe broadcasting at will
 *   - broadcast Vcc via ADC_MODE(ADC_VCC) and ESP.getVcc()
 *  
 *  Links:
 *   - Hall Effect Sensor: http://uk.rs-online.com/web/p/hall-effect-sensors/8223775/
 *   - Adafruit SHT31: https://learn.adafruit.com/adafruit-sht31-d-temperature-and-humidity-sensor-breakout/wiring-and-test
 *   - Wiki page with hardware: https://github.com/rswift/wifi-temperature-broadcast/wiki/External-Trigger-(Hall-Effect-Sensor)
 *  
 *  Robert Swift - September 2016
 */

#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "Adafruit_SHT31.h"

// disabled by default, but can be overridden via EEPROM and in the future, via the web server...
bool debugLogging = false;
bool verboseLogging = false;

// put this up near the top, to make it easy to spot for those not using the same baud rate as me for their FTDI Friend...
const unsigned int baudRate = 115200;

// comment out to remove all startup delay, I found it was necessary to let the board get itself together
const int startupDelay = 1500; //ms

// Set the WiFi and network details
IPAddress localIPAddress, subnetMask, multicastAddress(239,9,80,1);
word udpRemotePort = 9801; // for my own needs

// initialise the UDP objects, one for command & control, the other for Roastmaster
WiFiUDP udp;

// control protocol is very simple; will almost certainly expand at some point as i want to be able to control the main broadcast unit
char cncReadBeanMassProbe[] = "{\"command\":\"readProbes\",\"context\":\"Bean Mass\"}";
char cncBroadcastBeanMassReadings[] = "{\"command\":\"broadcastReadings\",\"context\":\"Bean Mass\"}";

// a check for EEPROM health and a simple struct to make reading/writing easier
const byte eepromComparison = B10101010;

struct eeprom_config {
  byte healthBight = !eepromComparison;
  char networkSSID[WL_SSID_MAX_LENGTH] = "SSID";
  char wifiPassword[WL_WPA_KEY_MAX_LENGTH] = "PASSWORD";
  bool debugEnabled = false;
  bool verboseEnabled = false;
};

// switch related data, force the switch off so no transmission until actually triggered
const int switchPin = 13;
volatile bool switchState = false;

// simple LED protocol for 
const int blueLed = 14;
const int redLed = 12;

// Temperature & Humidity sensor
Adafruit_SHT31 sht31 = Adafruit_SHT31();
bool shtSensorAvailable = false;

// set the reference for battery monitoring TODO - something with this!?
ADC_MODE(ADC_VCC);

void setup() {

  // seems to be needed to let things stabilise?
  delay(startupDelay);

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

  // configure LED's
  pinMode(blueLed, OUTPUT);
  pinMode(redLed, OUTPUT);

  // station mode then attempt connections until granted...
  if (debugLogging) { Serial.print(F("Attempting to connect to ")); Serial.print(config.networkSSID); if (verboseLogging) { Serial.print(F(" using password ")); Serial.print(config.wifiPassword); } }
  WiFi.mode(WIFI_STA);
  long connectDelay = 10;
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(config.networkSSID, config.wifiPassword);
    flashLED(blueLed, 1);
    if (debugLogging && verboseLogging) { Serial.print(F(".")); Serial.flush(); }
    delay(connectDelay);
    connectDelay += connectDelay;
  }

  localIPAddress = WiFi.localIP();
  subnetMask = WiFi.subnetMask();
  if (debugLogging) { Serial.print(F(" connected with IP address ")); Serial.print(localIPAddress); Serial.print(F(" netmask ")); Serial.print(subnetMask); Serial.print(F(" using broadcast address ")); Serial.println(multicastAddress); WiFi.printDiag(Serial); }

  // setup the Humidity & Temperature sensor
  if (!sht31.begin(0x44)) {
    if (sht31.begin(0x45)) {
      shtSensorAvailable = true;
    }
  } else {
    shtSensorAvailable = true;
  }

  // configure the switch
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeHigh, RISING);
  pinMode(switchPin, INPUT_PULLUP);

  // signal end of startup
  digitalWrite(redLed, HIGH);
  digitalWrite(blueLed, HIGH);
  delay(357);
  digitalWrite(blueLed, LOW);
  flashLED(blueLed, 2);
  delay(123);
  digitalWrite(redLed, LOW);
}

void loop() {
  int transmissionStatus;
  if (switchState == HIGH) {
    if (debugLogging) { Serial.print(F("Switch HIGH, transmitting readBeanMassProbe message: ")); }
    transmissionStatus = transmitControlMessage(cncReadBeanMassProbe);
    flashLED(redLed, 1);
  } else {
    if (debugLogging) { Serial.print(F("Switch LOW, transmitting broadcastReadings message: ")); }
    transmissionStatus = transmitControlMessage(cncBroadcastBeanMassReadings);
    flashLED(blueLed, 1);
  }

  if (transmissionStatus == 1) {
    if (debugLogging) { Serial.println(F("success")); }
  } else {
    if (debugLogging) { Serial.println(F("failed!")); }
    digitalWrite(redLed, HIGH);
    flashLED(blueLed, 5);
    digitalWrite(redLed, LOW);
  }

  // do a quick reading
  if (shtSensorAvailable) {
    float temperature = sht31.readTemperature();
    float humidity = sht31.readHumidity();
    if (debugLogging) { Serial.print(F("Temperature: ")); Serial.print(temperature); Serial.print(F("°C Relative humidity: ")); Serial.print(humidity); Serial.println(F("%")); }
    // TODO: broadcast this value?
  }

  // probably pointless...
  delay(100);
}

/*
 * Interrupt on change, so set the status to HIGH or LOW, keeps it quick
 * 
 * Any debouncing to be done with hardware: http://coder-tronics.com/switch-debouncing-tutorial-pt1/
 */
void switchStateChangeHigh() {
  switchState = HIGH;
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeLow, FALLING);
}
void switchStateChangeLow() {
  switchState = LOW;
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeHigh, RISING);
}

/*
 * Trivial function to transmit the control message
 */
int transmitControlMessage(char* cncMessageToSend) {
  udp.beginPacketMulticast(multicastAddress, udpRemotePort, WiFi.localIP());
  udp.write(cncMessageToSend);
  return udp.endPacket();
}

