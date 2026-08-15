#pragma once
#include <cstdint>

// Stub tempo-domain configuration for the library's own native tests, and the
// reference for the contract: a product WITH a tempo layer provides its real
// pedal_core_tempo_config.hpp on the include path, defining every constant
// below; a product without one provides no such header, and the tempo modules
// (tap_tempo, tempo_controller, tempo_led) compile to empty TUs.

inline constexpr float    BPM_MIN     = 20.0f;
inline constexpr float    BPM_MAX     = 300.0f;
inline constexpr float    BPM_DEFAULT = 120.0f;

inline constexpr uint32_t TAP_TEMPO_MAX_INTERVAL  = 2000u;  // ms; longer gap restarts the window
inline constexpr uint8_t  TAP_TEMPO_AVERAGE_TAPS  = 4u;     // window size for the average
inline constexpr uint32_t MIDI_SYNC_TIMEOUT_MS    = 2000u;  // clock silence that expires sync
