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
 *  up the board. I've now added an I2C OLED display that provides onboard and probe status
 *  information (temperature + battery), the display is the split yellow/blue one 
 *  
 *  Important note: This code has been optimised based on changes to the Adafruit_SHT31
 *  library, specifically the creation of a dual sensor read function (because reading the
 *  physical sensor returns both temperature and humidity, therefore calling each function
 *  just takes twice as long) but mainly the replacement of a 500ms delay with a 17ms delay
 *  based on the information in the datasheet... The performance improvement is tremendous
 *  with the display updating in what I'd deem to be 'real time' given the nature of the
 *  microprocessor, display etc.
 *  Refer to https://github.com/rswift/Adafruit_SHT31/tree/optimisation for details...
 *  
 *  ToDo:
 *   - consider 'manual override' switch to allow me to force probe broadcasting at will
 *  
 *  Links:
 *   - Hall Effect Sensor: http://uk.rs-online.com/web/p/hall-effect-sensors/8223775/
 *   - Adafruit SHT31: https://learn.adafruit.com/adafruit-sht31-d-temperature-and-humidity-sensor-breakout/wiring-and-test
 *   - Wiki page with hardware: https://github.com/rswift/wifi-temperature-broadcast/wiki/External-Trigger-(Hall-Effect-Sensor)
 *   - I2C OLED display: https://www.amazon.co.uk/gp/product/B0156CO5IE/
 *   - Adafruit GFX library: https://learn.adafruit.com/adafruit-gfx-graphics-library/graphics-primitives
 *   - ArduinoJSON: https://github.com/bblanchon/ArduinoJson
 *  
 *  Robert Swift - October 2016
 */

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// logging disabled by default, but can be overridden via EEPROM and in the future, via the web server...
bool debugLogging = false;
bool verboseLogging = false;
bool ledEnabled = false;

// put this up near the top, to make it easy to spot for those not using the same baud rate as me for their FTDI Friend...
const unsigned int baudRate = 115200;

// comment out to remove all startup delay, I found it was necessary to let the board get itself together
const int startupDelay = 1500; //ms

// Set the WiFi and network details
IPAddress localIPAddress, subnetMask, multicastBroadcastAddress(239,9,80,1), multicastListenerAddress(239, 31, 8, 55);
word udpListenerPort = 31855; // for my own needs
word udpBroadcastPort = 9801; // for my own needs

// initialise the UDP objects, one for command & control, the other for my broadcaster
WiFiUDP probeUdp;
WiFiUDP cncUdp;

// control protocol is very simple; will almost certainly expand at some point as i want to be able to control the main broadcast unit
char cncReadBeanMassProbe[] = "{\"command\":\"readProbes\",\"context\":\"Bean Mass\"}";
char cncBroadcastBeanMassReadings[] = "{\"command\":\"broadcastReadings\",\"context\":\"Bean Mass\"}";

// a check for EEPROM health and a simple struct to make reading/writing easier
const byte eepromComparison = B10101010;

struct eeprom_config {
  byte healthBight = !eepromComparison;
  char networkSSID[WL_SSID_MAX_LENGTH] = "SSID";
  char wifiPassword[WL_WPA_KEY_MAX_LENGTH] = "password";
  bool debugEnabled = false;
  bool verboseEnabled = false;
  bool ledEnabled = false;
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
sht31readings sensorReadings;

// Trigger status data
const byte batteryStartX = 99, triggerBatteryY = 2, probeBatteryY = 18;
byte updateTriggerBattery = 10;

#define OLED_RESET 15 // not used by my board, so set to an unused GPIO pin...
Adafruit_SSD1306 display(OLED_RESET);
#if (SSD1306_LCDHEIGHT != 64)
#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif

// set the reference for battery monitoring
ADC_MODE(ADC_VCC);

void setup() {

  // seems to be needed to let things stabilise?
  delay(startupDelay);

  Wire.pins(4, 5); Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3D (for the 128x64)
  display.clearDisplay();
  display.display(); delay(1);
  display.setTextSize(1); display.cp437(); display.setTextColor(WHITE, BLACK);

  initaliseTriggerDisplay();
  setTriggerStatusText("Initialising", false);
  display.display(); delay(5);

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
  ledEnabled = config.ledEnabled;

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
    setTriggerStatusText("WiFi Delay!", false);
    delay(connectDelay);
    connectDelay += connectDelay;
  }

  setTriggerStatusText("WiFi Connected", false);
  localIPAddress = WiFi.localIP();
  subnetMask = WiFi.subnetMask();
  if (debugLogging) { Serial.print(F(" connected with IP address ")); Serial.print(localIPAddress); Serial.print(F(" netmask ")); Serial.print(subnetMask); Serial.print(F(" using broadcast address ")); Serial.println(multicastBroadcastAddress); WiFi.printDiag(Serial); }

  // setup the Humidity & Temperature sensor
  if (!sht31.begin(0x44)) {
    if (sht31.begin(0x45)) {
      shtSensorAvailable = true;
    }
  } else {
    shtSensorAvailable = true;
  }

  // configure the Command & Control UDP listener
  probeUdp.beginMulticast(WiFi.localIP(), multicastListenerAddress, udpListenerPort);

  // configure the switch
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeHigh, RISING);
  pinMode(switchPin, INPUT_PULLUP);

  // signal end of startup
  setTriggerStatusText("Ready", true);
  digitalWrite(redLed, HIGH);
  digitalWrite(blueLed, HIGH);
  delay(357);
  digitalWrite(blueLed, LOW);
  flashLED(blueLed, 2);
  delay(123);
  digitalWrite(redLed, LOW);

  // only draw the probe lines after we're ready to go...
  initaliseProbeDisplay();
}

void loop() {
  int transmissionStatus;
  if (switchState == HIGH) {
    if (debugLogging) { Serial.print(F("Switch HIGH, transmitting readBeanMassProbe message: ")); }
    transmissionStatus = transmitControlMessage(cncReadBeanMassProbe);
    setTriggerStatusText("Read...", true);
    flashLED(redLed, 1);
  } else {
    if (debugLogging) { Serial.print(F("Switch LOW, transmitting broadcastReadings message: ")); }
    transmissionStatus = transmitControlMessage(cncBroadcastBeanMassReadings);
    setTriggerStatusText("Transmit", true);
    flashLED(blueLed, 1);
  }

  if (transmissionStatus == 1) {
    if (debugLogging) { Serial.println(F("success")); }
    setTriggerStatusText("OK", true);
  } else {
    if (debugLogging) { Serial.println(F("failed!")); }
    setTriggerStatusText("ERROR!", true);
    digitalWrite(redLed, HIGH);
    flashLED(blueLed, 5);
    digitalWrite(redLed, LOW);
  }

  // do a quick reading
  if (shtSensorAvailable) {
    sensorReadings = sht31.readSensors();
//    temperature = sht31.readTemperature();
//    humidity = sht31.readHumidity();
    if (debugLogging) { Serial.print(F("Temperature: ")); Serial.print(sensorReadings.temperature); Serial.print(F("°C Relative humidity: ")); Serial.print(sensorReadings.humidity); Serial.println(F("%")); }
  }

  // update the display, the top line (rows 0-15) are the trigger (just the trigger battery, every 10 iterations), rows 16-63 give probe details
  if (--updateTriggerBattery == 0) {
    drawTriggerBattery();
    updateTriggerBattery = 9;
  }
  updateProbeStatus();
}

// Interrupt on change, so set the status to HIGH or LOW, keeps it quick
void switchStateChangeHigh() {
  switchState = HIGH;
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeLow, FALLING);
}
void switchStateChangeLow() {
  switchState = LOW;
  attachInterrupt(digitalPinToInterrupt(switchPin), switchStateChangeHigh, RISING);
}

// Trivial function to transmit the control message
int transmitControlMessage(char* cncMessageToSend) {
  cncUdp.beginPacketMulticast(multicastBroadcastAddress, udpBroadcastPort, WiFi.localIP());
  cncUdp.write(cncMessageToSend);
  return cncUdp.endPacket();
}
