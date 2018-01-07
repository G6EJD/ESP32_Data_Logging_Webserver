# ESP32_Data_Logging_Webserver
An ESP32/ESP8266 and SHT30 that together form a data logger that is accessed via a webserver and data is graphed via google Charts.

Put both files (ESP32_SHT30_SPIFFS_DataLogger_01.ino and credentials.h) in your sketch folder and modifiy the Wi-Fi access requirements (password and SSID) to match yours via the file tab in the IDE.

Monitor the Serial Port for the assigned IP address and connect tothe server with http://IP/ e.g. http://192.168.0.5/


