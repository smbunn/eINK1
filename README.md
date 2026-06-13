# eINK1 - e-Ink Weather Station with LoRaWAN

Arduino-based e-ink weather station project for the Heltec Automation V3 board. Fetches weather data from Open-Meteo, displays a 5-day forecast on an e-ink display, and sends telemetry via LoRaWAN.

## Hardware

- Heltec Automation V3 board (ESP32 + LoRa)
- E-ink display (HT E0213A367, 2.13" e-ink)
- Deep sleep power management for battery efficiency
<img width="8160" height="4590" alt="PXL_20251128_232951582" src="https://github.com/user-attachments/assets/f0a2f27e-7414-4641-8664-cb11e9711027" />


## Features

- Deep sleep (~2 min wake cycles) for display time updates
- Full weather fetch every 30 minutes via WiFi + Open-Meteo API
- LoRaWAN uplink with session management
- 5-day forecast cache in RTC memory
- Watchdog timer for robustness
- Auckland timezone (NZST/NZDT)

## Files

| File | Description |
|------|-------------|
| `weather_station_LoRa_eINK1.ino` | Main Arduino sketch (~800 lines) |
| `board-config.h` | Board pin definitions and LoRa region settings |
| `HT_E0213A367.h` | E-ink display driver (2.13" 367x213 resolution) |
| `images.h` | Embedded bitmap assets (weather icons, UI elements) |
| `secrets.h` | Modify this file to add your WiFI details and OTAA for LoRa details

## Dependencies

- Arduino IDE with ESP32 board support
- ArduinoJson
- LoRaWan_APP library
- Heltec V3 board definitions

## Deployment

1. Open `weather_station_LoRa_eINK1.ino` in Arduino IDE
2. Set board to Heltec V3 (ESP32)
3. Update WiFi credentials in sketch if needed
4. Compile and upload

## Author

Simon Bunn
