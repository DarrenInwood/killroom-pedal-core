#pragma once
#include <cstdint>

// Stub external-input configuration for the library's own native tests, and
// the reference for the contract: a product WITH the jack provides its real
// pedal_core_extinput_config.hpp defining these; a product without one
// provides no such header, and external_input compiles to an empty TU.

// The count tracks external_input::Action, which a static_assert there pins: the
// vocabulary grew when the footswitches became musical (momentary bypass, freeze,
// rotary speed and brake, compare), and again when a held freeze and a latching one
// were told apart.
inline constexpr uint8_t EXT_ACTION_COUNT        = 14u;
inline constexpr uint8_t EXT_DEFAULT_TIP_ACTION  = 4u;  // Preset Down
inline constexpr uint8_t EXT_DEFAULT_RING_ACTION = 3u;  // Preset Up
inline constexpr uint8_t EXT_DEFAULT_BOTH_ACTION = 2u;  // Tap tempo
