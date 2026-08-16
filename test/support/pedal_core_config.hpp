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

// The device's page size. A write may not cross a page boundary, and assuming a
// LARGER page than the part has makes it wrap and corrupt — so this is the
// product's to state, not the library's to guess.
inline constexpr uint16_t EEPROM_PAGE_BYTES = 64u;

// What the dead-part RAM mirror holds. A product whose store fits in RAM sets
// HEAD_END and TAIL_BASE to 0 and mirrors the map whole; one whose store does
// not names the two ends worth keeping — the header and the system blocks — and
// a window big enough for one record of the bulk region between them.
//
// These are deliberately set to the PARTIAL shape rather than the degenerate one, so the
// library's own tests exercise the routing. The all-zero shape (mirror everything) is the
// simpler path and is what a product with a small store uses; it is a compile-time branch,
// so only one of the two can be built at a time and this is the one worth pinning.
inline constexpr uint16_t EEPROM_MIRROR_HEAD_END   = 0x0040u;  // the layout header
inline constexpr uint16_t EEPROM_MIRROR_TAIL_BASE  = 0x7A00u;  // settings, cal, ring
inline constexpr uint16_t EEPROM_MIRROR_SLOT_BYTES = 64u;      // one record of the bulk

// Display geometry (display.cpp).
inline constexpr uint8_t OLED_WIDTH  = 128u;
inline constexpr uint8_t OLED_HEIGHT = 64u;
inline constexpr uint8_t OLED_PAGES  = OLED_HEIGHT / 8u;
