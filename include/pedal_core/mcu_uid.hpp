#pragma once
#include <cstdint>

// The MCU's factory unique ID, and the string form a USB descriptor carries.
//
// Header-only on purpose. A product's application reads the UID through a linkable
// `read()` seam so host tests can substitute a deterministic one, but a bootloader is a
// separate binary that links only header-only shared code — and the bootloader is
// precisely where the UID matters most, because a device in DFU has no other way to say
// which unit it is.
//
// The serial-number string produced here has to be IDENTICAL in a product's application
// and its bootloader. If the two differ, the pedal appears to vanish and a different
// device appears whenever it enters DFU, and any host tracking "the unit I was talking
// to" across that reboot loses it. Since a bootloader is typically not field-updatable,
// its spelling is the one that is frozen and the application's is the one that must keep
// conforming — which is why both sides call this function rather than each formatting
// their own.
namespace pedal_core::mcu_uid {

inline constexpr uint8_t LENGTH = 12u;          // 96 bits
inline constexpr uint8_t HEX_CHARS = 24u;       // LENGTH * 2, excluding the terminator

// STM32F4 unique device ID register. Other families put it elsewhere, so a product on a
// different part defines this before including the header.
#ifndef PEDAL_CORE_UID_BASE
#define PEDAL_CORE_UID_BASE 0x1FFF7A10u
#endif

// Copy LENGTH bytes from the ID register at `base`, least significant word first.
inline void read_at(uint32_t base, uint8_t* out)
{
    const volatile uint8_t* uid = reinterpret_cast<const volatile uint8_t*>(base);
    for (uint8_t i = 0; i < LENGTH; ++i) out[i] = uid[i];
}

// Format LENGTH bytes as uppercase hex into `out`, which must hold HEX_CHARS + 1.
//
// Byte order is the order read_at() produces, which is also the order the SysEx UID
// frames use — so the string a host sees on the USB port and the UID it addresses the
// pedal with over MIDI are the same bytes in the same order, and correlating the two is
// a string compare rather than a byte-swap someone has to remember.
inline void to_hex(const uint8_t* uid, char* out)
{
    static const char k_hex[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < LENGTH; ++i) {
        out[i * 2u]      = k_hex[(uid[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = k_hex[uid[i] & 0x0Fu];
    }
    out[HEX_CHARS] = '\0';
}

}  // namespace pedal_core::mcu_uid
