Pre-requisite setup
1. Open Sketch in `coop\Arduino\coop\DHT11_WebSockets_LittleFS\DHT11_WebSockets_LittleFS.ino`.
2. Set Port: Tools > Port > COM4
3. Set Board: Tools > Board > esp8266 > NodeMCU 1.0 (ESP-12E Module)
4. (Optional) Set Flash Size: Tools > Flash size > 4MB (FS:2MB OTA:~1019KB) or 4MB (FS:3MB OTA:~512KB)
Note: you may need to do these steps when freshly opening Arduino IDE


Sketch (DHT11_WebSockets_LittleFS.ino)

Setup
Search the following to configure:
1. const char* ssid > WiFi name
2. const char* password > WiFi password
3. const char* MQTT_TOPIC > Topic name (currently: 6d3647b111db22048d788f62b455eac1da5d3da9561cb8b7772959438eabbec3/data)
4. const int FAN_PIN > Fan port (currently: port 5 or D1)
5. const int LIGHT_PIN > Light port (currently: port 14 or D5)
6. float FAN_ON_TEMP_C or float FAN_OFF_TEMP_C > Fan ON and OFF temps
7. float LIGHT_ON_TEMP_C or float LIGHT_OFF_TEMP_C > Light ON and OFF temps
8. const unsigned long LIGHT_ON_DURATION_MS > light ON timer (currently: 3600000UL for 1 hour)
9. const unsigned long DATA_RATE_SECONDS > rate of posting new data (currently: 60, must be low as ESP has low memory)

To upload Sketch:
1. Sketch > Upload
Note: Ensure to close all open bottom tabs (ie. LittleFS, Serial Monitor etc)



Local index.html

Setup:
1. Dashboard can be found in start up logs in IDE or http://10.0.0.67/
2. Data history are stored in http://10.0.0.67/history
3. This HTML should be placed inside `data` folder of the project. The project should have the Sketch `.ino` file.

To upload local HTML:
1. Close all open bottom tabs (ie. LittleFS, Serial Monitor etc)
2. Open command pallete: CTRL + SHIFT + P > Upload LittleFS to Pico


GIT index.html

Setup:
1. Dashboard can be found on https://migueldanminoza.github.io/coop/
2. This HTML should be in root of the `coop` project in the GIT repo.
