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
constexpr uint32_t HEADER_SIZE   = 16u;          // magic + size + ident + version

// Descriptor words, little-endian at HEADER_OFFSET:
//
//   +0x00  magic    MAGIC
//   +0x04  size     total image length, patched post-link
//   +0x08  ident    [7:0] IDENT_FORMAT | [15:8] manufacturer | [23:16] device | [31:24] --
//   +0x0C  version  [7:0] patch | [15:8] minor | [23:16] major | [31:24] --
//
// The two high bytes marked -- are MUST-IGNORE: a reader has to mask them off rather than
// compare the whole word. They are the only room a bootloader that has already shipped
// will ever have to accept something new, so ignoring them is a contract, not slack.
//
// A product that has not claimed the ident word leaves it 0xFFFFFFFF (erased flash), which
// reads as IDENT_FORMAT 0xFF and is what identity_matches() rejects. Nothing here changes
// validate(): integrity and identity stay separate predicates, so a product whose images
// are already sealed and in the field keeps validating exactly as before and adopts the
// identity check on its own first unit.
constexpr uint32_t IDENT_OFFSET   = 8u;
constexpr uint32_t VERSION_OFFSET = 12u;
constexpr uint8_t  IDENT_FORMAT   = 1u;

constexpr uint32_t make_ident(uint8_t manufacturer, uint8_t device)
{
    return (uint32_t)IDENT_FORMAT
         | ((uint32_t)manufacturer << 8)
         | ((uint32_t)device << 16);      // [31:24] left 0: reserved, must-ignore
}

constexpr uint32_t make_version(uint8_t major, uint8_t minor, uint8_t patch)
{
    return (uint32_t)patch
         | ((uint32_t)minor << 8)
         | ((uint32_t)major << 16);       // [31:24] left 0: build flags, must-ignore
}

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

// --- Identity ---------------------------------------------------------------------
// Deliberately not folded into validate(). validate() is the integrity predicate the
// build-side sealer and the CI gate reproduce byte for byte; identity is a bootloader's
// policy about which images it will run. Keeping them apart is what makes this word a
// pure addition for a product that has not claimed it yet.

inline uint8_t ident_format(const uint8_t* base)
{
    return (uint8_t)(rd32_le(base + HEADER_OFFSET + IDENT_OFFSET) & 0xFFu);
}

inline uint8_t ident_manufacturer(const uint8_t* base)
{
    return (uint8_t)((rd32_le(base + HEADER_OFFSET + IDENT_OFFSET) >> 8) & 0xFFu);
}

inline uint8_t ident_device(const uint8_t* base)
{
    return (uint8_t)((rd32_le(base + HEADER_OFFSET + IDENT_OFFSET) >> 16) & 0xFFu);
}

inline uint8_t version_major(const uint8_t* base)
{
    return (uint8_t)((rd32_le(base + HEADER_OFFSET + VERSION_OFFSET) >> 16) & 0xFFu);
}

inline uint8_t version_minor(const uint8_t* base)
{
    return (uint8_t)((rd32_le(base + HEADER_OFFSET + VERSION_OFFSET) >> 8) & 0xFFu);
}

inline uint8_t version_patch(const uint8_t* base)
{
    return (uint8_t)(rd32_le(base + HEADER_OFFSET + VERSION_OFFSET) & 0xFFu);
}

// True when the image names this product. An unclaimed descriptor (0xFFFFFFFF, erased
// flash) fails on the format byte, so a bootloader that enforces this must ship on the
// product's first unit -- there is no way to add the check later for pedals already in
// the field, because their bootloader will never perform it.
//
// The version is deliberately NOT part of this. Refusing an older image would make a
// downgrade unrecoverable on a product whose bootloader cannot be updated, which is a
// bricking policy, not a safety one; the version is carried so a host can report it.
inline bool identity_matches(const uint8_t* base, uint8_t manufacturer, uint8_t device)
{
    return ident_format(base) == IDENT_FORMAT
        && ident_manufacturer(base) == manufacturer
        && ident_device(base) == device;
}

}  // namespace app_image
