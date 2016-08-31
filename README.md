# WiFi Temperature Broadcast
This is a simple application, firmly inspired by Roasthacker to capture and broadcast the bean mass temperature from my Gene Café CBR-101 coffee roaster. The ultimate goal is to feed these temperatures into the Roastmaster Data Logger for hands-free temperature capture directly on the iPad…

Incorporates the great work done by @rainfroginc to create his Roastmaster Datagram Protocol - I'm very grateful to Danny for his support getting this far, and it is exciting to be helping him develop Roastmaster too

It has been written and tested on the Adafruit HUZZAH ESP8266 with the Adafruit MAX31855 plus K-Type Thermocouple. It is therefore based on the many, excellent example sketches available within the Arduino IDE and elsewhere…

The payload is formatted as JSON, but is described in a method so can be changed as desired
 
This stuff really is amazing, when I think how big the 300 baud modems I used to provide remote support for customers around the globe, to be able to get this much capability into a matchbox is phenomenal :)

This code is issued under the “Bill and Ted - be excellent to each other” licence (which has no content, just, well, be excellent to each other), however, I have included the licence files from my original starting file (serialthermocouple.ino) at the bottom of this sketch…

# Usage
- set the probeName, WiFi and UDP broadcast port settings
- ensure the MAX31855 pins are assigned correctly
- tinker with the poll rates, debug/verbose and baud rate values as you prefer
- flash the ESP8266
- monitor your network for UDP datagrams sent to the port specified by udpRemotePort
- and/or consume the datagram in a suitable application

# ToDo
- ~~understand the impact of fluctuations of the cold junction temperature~~ just solder the thermocouple wire to the MAX31855, solves the problem!
- remove hardware tilt switch trigger as this is not sufficiently accurate, replace with hall effect switch and a WiFi triggered command
- build a small server element into the code to receive instructions (broadcast now, configuration etc.)
- dynamic WiFi settings to permit the client application to govern settings such as SSID & password, poll/broadcast rates etc. and store in EEPROM
- test on other boards
- simplify the code, maybe separate out although it is handy having it all in a single file
- ~~add a tilt switch to mean that the temperature readings are taken when the probe is actually in the bean mass, then broadcast afterwards~~
 
# Some links
**Roastmaster**: https://github.com/rainfroginc/Roastmaster_RDP_Probe_Host_For_SBCs & https://itunes.apple.com/gb/app/roastmaster/id375526217?mt=8

**Roasthacker**: http://roasthacker.com/?p=529 & http://roasthacker.com/?p=552

**Adafruit ESP8266**: https://www.adafruit.com/products/2471

**Adafruit MAX31855**: https://www.adafruit.com/products/269 & https://cdn-shop.adafruit.com/datasheets/MAX31855.pdf

**Using a thermocouple**: https://learn.adafruit.com/thermocouple/using-a-thermocouple

**Calibration**: https://learn.adafruit.com/calibrating-sensors/maxim-31855-linearization

**TCP dump or Wireshark for packet capture**: http://www.tcpdump.org/tcpdump_man.html or https://www.wireshark.org/

**Timer**: http://www.switchdoc.com/2015/10/iot-esp8266-timer-tutorial-arduino-ide/

Robert Swift - August 2016.
