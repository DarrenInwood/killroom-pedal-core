#pragma once
#include <cstdint>

// Stub product configuration for the library's own native tests. A consuming
// product provides its real pedal_core_config.hpp on the include path ahead of
// test/support; the values here exist only so the library compiles and its
// tests can pin behaviour against known constants.

// Parameter scale (adc_map).
inline constexpr uint8_t  PARAM_VALUE_BITS = 10u;
inline constexpr uint16_t PARAM_MAX        = (1u << PARAM_VALUE_BITS) - 1u;
inline constexpr uint16_t PARAM_MID        = PARAM_MAX / 2u;        // 511
inline constexpr uint16_t PARAM_CENTRE     = (PARAM_MAX + 1u) / 2u; // 512, bipolar neutral

// EEPROM geometry (eeprom.cpp): the addressable stored map, and the dedicated
// health-probe scratch byte outside it.
inline constexpr uint16_t EEPROM_STORE_SIZE = 0x7E00u;
inline constexpr uint16_t EEPROM_PROBE_ADDR = 0x7FFFu;

// Display geometry (display.cpp).
inline constexpr uint8_t OLED_WIDTH  = 128u;
inline constexpr uint8_t OLED_HEIGHT = 64u;
inline constexpr uint8_t OLED_PAGES  = OLED_HEIGHT / 8u;
