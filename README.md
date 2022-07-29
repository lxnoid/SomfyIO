# SomfyIO 
Source code to controll the Somfy remote over Wifi with MQTT.
STL files for the case and further assembly instructions are given under this link: https://www.thingiverse.com/thing:4847509

Steps:
- Download code from github
- Compile code
- Create littleFS after updating https://github.com/lxnoid/SomfyIO/blob/c1a4ee1ab20e0cd95213c6cc1ef700d739b76f50/data/config.json with proper Wifi and MQTT setup
- Flash application binary to ESP32 d1 mini
- Flash filesystem with config.json to ESP32 d1 mini
- Connect remote according: https://github.com/lxnoid/SomfyIO/blob/c1a4ee1ab20e0cd95213c6cc1ef700d739b76f50/SomfyIOZeichnung.svg with printed adapter and pogopins, or solder directly.
