#pragma once

#include <array>
#include <cstdint>

namespace board {
#if defined(DATV_BOARD_POE_ISO)
constexpr const char* name = "Olimex ESP32-POE-ISO (WROOM)";
constexpr std::array<uint8_t, 6> alarmPins{{4, 13, 14, 32, 33, 36}};
constexpr bool ethernet = true;
// Receiver HIGH drives an optocoupler; its output sinks the ESP32 pin LOW.
constexpr bool activeHigh = false;
#elif defined(DATV_BOARD_WIFI)
constexpr const char* name = "ESP32 DevKit / Wi-Fi bench profile";
constexpr std::array<uint8_t, 6> alarmPins{{16, 17, 18, 19, 21, 22}};
constexpr bool ethernet = false;
constexpr bool activeHigh = true;
#else
#error "Select an explicit supported hardware profile in platformio.ini"
#endif
}  // namespace board
