#pragma once
#include <cstdint>

// Host-portable SysEx firmware-update codec for the DFU bootloader: the Roland-
// style 7-bit unpacking and the CRC32 image verify, factored out of main.cpp so
// the two correctness-critical pure functions can be unit-tested without the
// CMSIS / TinyUSB machinery that surrounds them. A wrong decode or CRC would
// brick every firmware update, so these are locked down in test_sysex_codec.
// main.cpp includes this and routes its chunk decode + end-of-firmware verify
// through these helpers.
namespace sysex_codec {

// ---------------------------------------------------------------------------
// 7-bit SysEx decoding (Roland-style packing)
// 8 sysex bytes → 7 binary bytes
// ---------------------------------------------------------------------------
// The packing direction, for a frame the pedal SENDS rather than receives.
//
// The bootloader only ever decodes, which is why this arrived later than its
// inverse: the application needs it to put binary on the wire — the UID reply
// carries a 96-bit MCU serial that has no 7-bit-safe form otherwise.
//
// Each group of up to 7 binary bytes becomes a leading MSB byte followed by the
// low 7 bits of each, where bit j of the MSB byte is bit 7 of input byte j.
// Returns the encoded length, or 0 if `out_cap` could not hold it — a partial
// encode would put a frame on the wire that decodes to something else.
inline uint16_t encode_7bit(const uint8_t* in, uint8_t* out, uint16_t in_len, uint16_t out_cap)
{
    const uint16_t groups = (uint16_t)((in_len + 6u) / 7u);
    const uint16_t needed = (uint16_t)(in_len + groups);
    if (needed > out_cap) return 0u;

    uint16_t o = 0u;
    for (uint16_t i = 0; i < in_len; i += 7u) {
        const uint16_t left = (uint16_t)(in_len - i);
        const uint16_t n = (left < 7u) ? left : (uint16_t)7u;
        uint8_t msbs = 0u;
        for (uint16_t j = 0; j < n; ++j)
            if (in[i + j] & 0x80u) msbs = (uint8_t)(msbs | (uint8_t)(1u << j));
        out[o++] = msbs;
        for (uint16_t j = 0; j < n; ++j) out[o++] = (uint8_t)(in[i + j] & 0x7Fu);
    }
    return o;
}

// The encoded length `encode_7bit` produces for `in_len` binary bytes.
constexpr uint16_t encoded_size(uint16_t in_len)
{
    return (uint16_t)(in_len + (uint16_t)((in_len + 6u) / 7u));
}

inline uint16_t decode_7bit(const uint8_t* in, uint8_t* out, uint16_t in_len, uint16_t out_cap)
{
    uint16_t out_len = 0;
    for (uint16_t i = 0; i < in_len && out_len < out_cap; i += 8) {
        const uint8_t msbs = in[i];
        // Each group is one MSB byte followed by up to 7 data bytes. The final
        // group may be short when the encoded length isn't a multiple of 8
        // (e.g. a 256-byte chunk packs to 36 full groups + a 5-byte partial), so
        // the loop bound must admit that short final group — clamping the data
        // count to whatever remains (`avail` below) rather than requiring 7.
        const uint16_t avail = (uint16_t)(in_len - i - 1u);
        const uint16_t n = avail < 7u ? avail : 7u;
        // out_cap bounds the write: encoded_len comes from the SysEx header and is
        // only checked against the received-message size (up to SYSEX_MAX), not the
        // fixed decode buffer, so a malformed/oversized chunk must not overrun `out`.
        for (uint16_t j = 0; j < n && out_len < out_cap; ++j) {
            out[out_len++] = in[i + 1 + j] | ((msbs >> j & 1u) << 7);
        }
    }
    return out_len;
}

// The number of binary bytes decode_7bit unpacks from `in_len` encoded bytes when
// the output sink is large enough (no out_cap clamp). Each 8-byte group carries one
// MSB byte + up to 7 data bytes, so a full group yields 7 and a short final group of
// r bytes (1..8) yields r-1. A caller with a fixed decode buffer uses this to reject a
// chunk that would decode past the buffer before decoding into it — decode_7bit itself
// only clamps (silently dropping the overflow), which a chunk guard must not rely on.
inline uint16_t decoded_size(uint16_t in_len)
{
    const uint16_t full = in_len / 8u;
    const uint16_t rem  = in_len % 8u;
    return (uint16_t)(full * 7u + (rem ? rem - 1u : 0u));
}

// ---------------------------------------------------------------------------
// CRC32 (Ethernet polynomial, standard)
// ---------------------------------------------------------------------------
// `len` and the index are uint32_t: the whole-image verify passes the firmware
// size, which exceeds 65535 once the app grows past 64 KB. A uint16_t length
// would truncate the CRC range, and a uint16_t index would wrap and loop forever.
inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, uint32_t len)
{
    crc ^= 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFF;
}

} // namespace sysex_codec
