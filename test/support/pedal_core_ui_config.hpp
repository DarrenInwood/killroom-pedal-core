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

// Transient-overlay dwell (ui/compositor.cpp): how long the param focus panel
// and system banners stay up.
inline constexpr uint32_t DISPLAY_PARAM_SHOW_MS = 750u;

// The NRPN / RPN block (pedal_base.cpp, midi_responder_base.cpp) — standard
// CC numbers. RPN and the data inc/dec pair are consumed, not acted on: they
// disarm the NRPN latch so a host's RPN traffic cannot write a parameter.
inline constexpr uint8_t MIDI_CC_DATA_ENTRY_MSB = 6u;
inline constexpr uint8_t MIDI_CC_DATA_ENTRY_LSB = 38u;
inline constexpr uint8_t MIDI_CC_DATA_INC       = 96u;
inline constexpr uint8_t MIDI_CC_DATA_DEC       = 97u;
inline constexpr uint8_t MIDI_CC_NRPN_LSB       = 98u;
inline constexpr uint8_t MIDI_CC_NRPN_MSB       = 99u;
inline constexpr uint8_t MIDI_CC_RPN_LSB        = 100u;
inline constexpr uint8_t MIDI_CC_RPN_MSB        = 101u;
inline constexpr uint8_t NRPN_BANK_PARAMS       = 0u;

// Bank select, held for the next Program Change (pedal_base.cpp).
inline constexpr uint8_t MIDI_CC_BANK_MSB = 0u;
inline constexpr uint8_t MIDI_CC_BANK_LSB = 32u;

// SysEx identity (pedal_base.cpp validates the header; the device byte is a
// per-product hook).
inline constexpr uint8_t SYSEX_MANUFACTURER_ID = 0x7Du;

// The parameter scale reaches the library through pedal_core_config.hpp, the
// same as on a real product, where both shims funnel to one product header.
#include "pedal_core_config.hpp"
