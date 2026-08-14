#pragma once
#include <cstdint>
#include "sysex_codec.hpp"

// Boot-time application-image integrity check for the DFU bootloader.
//
// The app build emits a fixed descriptor (magic + size) at HEADER_OFFSET past the
// app flash base (firmware/src/app/app_image.cpp, pinned by the app linker script),
// and the production post-build step appends a CRC32 trailer over the whole image
// (firmware/tools/appimage_crc.py). validate() re-derives that CRC and rejects an
// image that is incomplete or corrupt — the state a firmware update aborted
// mid-flash leaves behind — so the bootloader can stay in DFU and prompt the user
// instead of jumping into a half-written app that would only crash-loop.
//
// Factored out like sysex_codec so the correctness-critical logic is host-unit-
// tested (test_app_image) without the CMSIS/flash machinery. Contract constants
// mirror config/pinmap.hpp on the app side.
namespace app_image {

constexpr uint32_t MAGIC         = 0x4D4F4448u;  // marker bytes 48 44 4F 4D
constexpr uint32_t HEADER_OFFSET = 0x200u;       // descriptor offset from the image base
constexpr uint32_t HEADER_SIZE   = 16u;          // magic + size + two reserved words

// Read a little-endian 32-bit word byte-wise: the descriptor and trailer are always
// word-aligned in flash, but a host test buffer need not be, and this also matches
// the byte-oriented style of the shared CRC.
inline uint32_t rd32_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Verify the application image at `base`. `region_bytes` is the usable size of the
// app flash partition, bounding a plausible image. Returns true only if the
// descriptor magic matches, `size` is sane and word-aligned, and the CRC32 over
// [base, base + size - 4) equals the trailer word at base + size - 4. Reads at most
// `size` bytes from `base`.
inline bool validate(const uint8_t* base, uint32_t region_bytes)
{
    if (rd32_le(base + HEADER_OFFSET) != MAGIC) return false;

    const uint32_t size = rd32_le(base + HEADER_OFFSET + 4u);
    // Must be large enough to hold the descriptor + trailer, fit the partition, and
    // be word-aligned (the build pads to a 4-byte boundary before appending the CRC).
    if (size < HEADER_OFFSET + HEADER_SIZE + 4u) return false;
    if (size > region_bytes) return false;
    if (size & 3u) return false;

    const uint32_t body_len   = size - 4u;
    const uint32_t stored_crc = rd32_le(base + body_len);
    const uint32_t calc_crc   = sysex_codec::crc32_update(0, base, body_len);
    return calc_crc == stored_crc;
}

}  // namespace app_image
