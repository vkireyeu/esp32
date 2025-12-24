### Some cketches for ESP32 boards

#### `esp32-s3-sensors/`
Board: ESP32-S3-WROOM-1 N16R8  
Sensors:
- Sensirion SHT40: temperature, humidity
- Winsen MH-Z19C: CO2 ppm
- Winsen ZH03B: PM1.0, PM2.5, PM10

In the sketch:
- "Captive portal" for settings
- OTA
- Sensors reading
- MQTT publish
