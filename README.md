(c) 2025 M10tech

# Bus Display Driver

use the LCM4ESP32 concept to run this  
Details on https://github.com/HomeACcessoryKid/LCM4ESP32  
and instructions how to deploy here  
https://github.com/HomeACcessoryKid/LCM4ESP32/blob/main/deploy.md  

under developement so don't expect stability or anything

## Version History

### Initial commit

### 0.0.1 "proof of concept"
- can replicate some basic commands

### 0.0.2 moved to UART driver, not I2S
- turns out to be sufficient for this purpose
- copy from Quatt-Modbus repository
- reuse the setup for UART to do TX
- adjust for busdisplay 600-8E1
- add checksum function
- inherits LCM4ESP32, UDPlogger, pinger, SNTP

### 0.0.3 introduce MQTT subscriber
- topic bus_panel/message
- broker via CLI

### 0.0.4 use ota_string
- sets mqtt-uri
- and ping_target
- and target display

### 0.0.5 allow cli number input
- push this to a display message
