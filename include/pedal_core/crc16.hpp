#pragma once
#include <cstdint>

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR).
// The one integrity check shared by every EEPROM block (see config/eeprom_layout.hpp):
// each block stores this CRC little-endian over all preceding bytes, reserved
// bytes included. Bitwise (no table) — the longest span checked is one 128-byte
// block, far too small for table lookup to matter, and keeping it header-only
// lets host tests use the identical implementation.
// Check value: crc16_ccitt("123456789", 9) == 0x29B1.
inline uint16_t crc16_ccitt(const uint8_t* data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t b = 0; b < 8u; ++b)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}
