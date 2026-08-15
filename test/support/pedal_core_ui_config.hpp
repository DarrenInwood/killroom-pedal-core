#pragma once
#include <cstdint>

// Stub UI configuration for the library's own native tests, and the reference
// for the contract: a consuming product provides its real
// pedal_core_ui_config.hpp on the include path ahead of test/support, defining
// every constant below (usually by including its own narrow config headers).
// The values here exist so the library compiles and its tests can pin
// behaviour against known constants.

// Footswitch feel (footswitch.cpp).
inline constexpr uint32_t FOOTSWITCH_DEBOUNCE_MS = 20u;
inline constexpr uint32_t FOOTSWITCH_HOLD_MS     = 600u;

// MIDI receive (midi_handler.cpp): SysEx accumulation buffer, sized clear of
// the largest frame the product accepts.
inline constexpr uint16_t SYSEX_RX_BUF = 512u;
