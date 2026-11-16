#pragma once

// ESP32-S3 GPIO proposal. Verify the actual HW-261 board schematic and
// logic-level compatibility before connecting anything.
constexpr int PIN_SCK   = 12;
constexpr int PIN_MISO  = 13;
constexpr int PIN_MOSI  = 11;
constexpr int PIN_XCS   = 10;
constexpr int PIN_XDCS  = 9;
constexpr int PIN_DREQ  = 14;
constexpr int PIN_XRST  = 8;
constexpr int PIN_SDCS  = 4;
