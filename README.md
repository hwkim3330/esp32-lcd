# esp32-lcd — 회의실 예약 표시기 (Meeting-room reservation display)

An ESP32 sketch that shows the day's meeting-room reservations from **Dooray** on an LCD, with Korean text. Rooms are switched with on-screen navigation; a top bar shows the clock and date (NTP).

두레이(Dooray) 자원 예약 API에서 회의실 예약 현황을 받아 ESP32 LCD에 한글로 표시하는 스케치입니다.

## Hardware / libraries

- ESP32 board with an SPI LCD supported by **LovyanGFX** autodetect (`LGFX_AUTODETECT`)
- Arduino libraries: LovyanGFX, U8g2 (Korean unifont), ArduinoJson; WiFi / HTTPClient / SNTP from the ESP32 core

## Setup

Edit the *user config* block at the top of `1.ino`:

```cpp
static const char* WIFI_SSID    = "...";
static const char* WIFI_PASS    = "...";
static const char* DOORAY_TOKEN = "YOUR_TOKEN_HERE";   // personal Dooray API token
```

and the `ROOMS[]` table (Dooray resource IDs + labels). Then build and flash with Arduino IDE or `arduino-cli`.

Do not commit a real token.

**Status:** single-sketch prototype.
