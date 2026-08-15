#pragma once
#include <cstdint>
#include "crc16.hpp"

// The family's stored-block convention: every EEPROM block — preset record,
// settings, calibration, last-slot ring record, layout header — ends with a
// CRC-16/CCITT-FALSE over all preceding bytes, stored little-endian in the
// final two. The seal covers reserved 0xFF bytes too, so a later field can
// claim a reserved byte (0xFF as its uninitialised sentinel) and blocks
// written before that field existed still validate.
//
// Field layouts, magics and format versions are each product's own; this
// header owns only the seal.
namespace pedal_core::blocks {

inline void seal(uint8_t* buf, uint16_t size)
{
    const uint16_t crc = crc16_ccitt(buf, (uint16_t)(size - 2u));
    buf[size - 2u] = (uint8_t)(crc & 0xFFu);
    buf[size - 1u] = (uint8_t)(crc >> 8);
}

inline bool crc_ok(const uint8_t* buf, uint16_t size)
{
    const uint16_t stored = (uint16_t)(buf[size - 2u] | ((uint16_t)buf[size - 1u] << 8));
    return stored == crc16_ccitt(buf, (uint16_t)(size - 2u));
}

}  // namespace pedal_core::blocks
