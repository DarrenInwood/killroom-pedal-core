#pragma once
#include <cstdint>

// The DFU-over-SysEx wire contract, shared by every pedal in the family and by
// the host updater. The bootloader and the application both speak it — the app
// only to receive ENTER_BOOTLOADER and reboot into it — so the command bytes
// live here rather than in either one.
//
// Frames are  F0 <mfr> <device> <cmd> [payload] F7. The manufacturer byte is
// common to the family; each product picks its own device byte so two pedals
// can share a MIDI port.
//
// Commands are APPEND-ONLY: a host updater in the field speaks whatever
// version it shipped with.
//
//   Enter DFU (sent to the application). The payload is the target's 96-bit
//   MCU unique ID, 7-bit packed, and a pedal whose own ID does not match
//   ignores the frame:
//     F0 <mfr> <dev> 01 [uid12, 7-bit packed as 14 bytes] F7
//
//   The address is not optional. Over USB the command reaches one port and one
//   pedal, but over DIN every pedal downstream sees every byte, and two of a
//   model answer to the same device byte -- so an unaddressed reboot would put
//   a whole chain into DFU at once, and recovering them one at a time is worse
//   than not rebooting them. CMD_IDENTIFY is how a host tells apart the ones
//   that are already in DFU; holding both footswitches at power-on remains the
//   way in for a pedal that cannot be addressed at all.
//   Begin firmware — the total image size, so the bootloader can show a
//   percentage. Also resets the write session, so resending it starts a clean
//   retry after a failure:
//     F0 <mfr> <dev> 05 [size4, 7-bit packed as 5 bytes] F7
//   Firmware chunk. The address is four 7-bit groups (28 bits, which reaches
//   any flash address these parts have) and the length is the ENCODED payload
//   length in two 7-bit groups. Seven-bit groups throughout, because every
//   byte between F0 and F7 must be <= 0x7F -- a raw 8-bit address byte of 0x80
//   or above is a MIDI status byte and aborts the frame:
//     F0 <mfr> <dev> 02 [a27..21 a20..14 a13..7 a6..0]
//                       [enc_len_hi7 enc_len_lo7] [7-bit packed data] F7
//   End of firmware — CRC32 over the whole image:
//     F0 <mfr> <dev> 03 [crc4, 7-bit packed] F7
//   Status, from the bootloader after every command:
//     F0 <mfr> <dev> 7E [status] [addr3 addr2 addr1 addr0] F7
//   Identify. Request carries no payload; the reply carries INFO_LEN bytes:
//     F0 <mfr> <dev> 04 F7                      (host -> bootloader)
//     F0 <mfr> <dev> 04 [info...] F7            (bootloader -> host)
#include "sysex_codec.hpp"

namespace dfu_protocol {

inline constexpr uint8_t CMD_ENTER_BOOTLOADER = 0x01u;
inline constexpr uint8_t CMD_FW_CHUNK         = 0x02u;
inline constexpr uint8_t CMD_FW_END           = 0x03u;
// Identify: what are you, and what can you do?
//
// A bootloader in DFU is otherwise anonymous — two of a model on one MIDI port cannot be
// told apart, and a host has no way to ask what the pedal it is about to write to
// expects. That matters more than it looks, because a bootloader is typically not
// field-updatable: whatever it can answer on the day it ships is what every host will
// ever be able to negotiate with. So this exists from a product’s first unit or not at
// all, and a host that gets no reply must read that as "an older bootloader", never as
// "no pedal there".
//
// Request and reply share the command byte and are told apart by payload length: a
// device never sends an empty 0x04 and a host never sends a non-empty one. That
// asymmetry is load-bearing on a DIN chain, where a pedal sees the replies of every
// other pedal on the wire — without it, one identify request would start a reply storm.
inline constexpr uint8_t CMD_IDENTIFY          = 0x04u;
inline constexpr uint8_t CMD_FW_BEGIN         = 0x05u;
// The status reply's command byte. A product whose own MIDI header also
// defines a status command must use this value: it is what the bootloaders
// emit and what the host updater matches on.
inline constexpr uint8_t CMD_FW_STATUS        = 0x7Eu;

enum class Status : uint8_t {
    Ack      = 0x00u,   // chunk accepted and written
    Nack     = 0x01u,   // malformed, out of range, or a failed flash write
    CrcFail  = 0x02u,   // the image arrived but its CRC32 does not match
    Complete = 0x03u,   // verified; the bootloader is about to reboot
};

// The address a status frame reports back: for a chunk, where the next byte is
// expected — which is what lets a host resume rather than restart. It is packed
// as four 7-bit groups, like the chunk address it answers.
//   F0 mfr dev 7E status a27..21 a20..14 a13..7 a6..0 F7
inline constexpr uint8_t STATUS_FRAME_LEN = 10u;

// --- Identify reply ---------------------------------------------------------------
// Every byte is <= 0x7F, so binary fields are 7-bit packed exactly like the chunk
// address and the UID payload elsewhere in this protocol.
//
//   [0]      INFO_FORMAT
//   [1]      manufacturer
//   [2]      device
//   [3..5]   bootloader version: major, minor, patch
//   [6..19]  MCU unique ID, 12 bytes packed as 14
//   [20..24] app region base, 4 bytes packed as 5
//   [25..29] app region size, 4 bytes packed as 5
//   [30..31] max chunk payload in binary bytes, hi7 lo7
//   [32]     capability flags; 0 today, a host must ignore bits it does not know
//
// Two rules a host MUST follow, because the half that ages is the host, not the pedal:
//   1. Read [0] first and stop if it is a format you do not know, rather than parsing
//      fields at offsets that may have moved.
//   2. A reply LONGER than you expect is not an error. Read the fields you know by
//      offset and ignore the tail. This is the only way a later bootloader can add a
//      field without spending another command byte.
inline constexpr uint8_t INFO_FORMAT   = 1u;
inline constexpr uint8_t INFO_LEN      = 33u;

inline constexpr uint8_t INFO_OFF_FORMAT    = 0u;
inline constexpr uint8_t INFO_OFF_MFR       = 1u;
inline constexpr uint8_t INFO_OFF_DEVICE    = 2u;
inline constexpr uint8_t INFO_OFF_VERSION   = 3u;   // 3 bytes: major, minor, patch
inline constexpr uint8_t INFO_OFF_UID       = 6u;   // 14 bytes: 12 packed 7-bit
inline constexpr uint8_t INFO_OFF_APP_BASE  = 20u;  // 5 bytes: 4 packed 7-bit
inline constexpr uint8_t INFO_OFF_APP_SIZE  = 25u;  // 5 bytes
inline constexpr uint8_t INFO_OFF_CHUNK_MAX = 30u;  // 2 bytes: hi7 lo7
inline constexpr uint8_t INFO_OFF_FLAGS     = 32u;

struct Info {
    uint8_t        manufacturer;
    uint8_t        device;
    uint8_t        version_major;
    uint8_t        version_minor;
    uint8_t        version_patch;
    const uint8_t* uid;         // 12 raw bytes
    uint32_t       app_base;
    uint32_t       app_size;
    uint16_t       chunk_max;
    uint8_t        flags;
};

// Build the identify reply payload (the bytes between the command byte and F7).
// Returns the length written, or 0 if `cap` could not hold it — a partial payload would
// put a frame on the wire that decodes to something else.
inline uint16_t build_info(uint8_t* out, uint16_t cap, const Info& in)
{
    if (cap < INFO_LEN || in.uid == nullptr) return 0u;

    for (uint16_t i = 0; i < INFO_LEN; ++i) out[i] = 0u;
    out[INFO_OFF_FORMAT]     = INFO_FORMAT;
    out[INFO_OFF_MFR]        = (uint8_t)(in.manufacturer & 0x7Fu);
    out[INFO_OFF_DEVICE]     = (uint8_t)(in.device & 0x7Fu);
    out[INFO_OFF_VERSION + 0] = (uint8_t)(in.version_major & 0x7Fu);
    out[INFO_OFF_VERSION + 1] = (uint8_t)(in.version_minor & 0x7Fu);
    out[INFO_OFF_VERSION + 2] = (uint8_t)(in.version_patch & 0x7Fu);

    if (sysex_codec::encode_7bit(in.uid, out + INFO_OFF_UID, 12u, 14u) != 14u) return 0u;

    // Addresses go out as four 7-bit groups, the same 28-bit form the chunk address
    // uses — which reaches any flash address these parts have.
    const uint32_t vals[2] = { in.app_base, in.app_size };
    const uint8_t  offs[2] = { INFO_OFF_APP_BASE, INFO_OFF_APP_SIZE };
    for (uint8_t k = 0; k < 2u; ++k) {
        uint8_t raw[4] = {
            (uint8_t)((vals[k] >> 24) & 0xFFu), (uint8_t)((vals[k] >> 16) & 0xFFu),
            (uint8_t)((vals[k] >> 8) & 0xFFu),  (uint8_t)(vals[k] & 0xFFu),
        };
        if (sysex_codec::encode_7bit(raw, out + offs[k], 4u, 5u) != 5u) return 0u;
    }

    out[INFO_OFF_CHUNK_MAX + 0] = (uint8_t)((in.chunk_max >> 7) & 0x7Fu);
    out[INFO_OFF_CHUNK_MAX + 1] = (uint8_t)(in.chunk_max & 0x7Fu);
    out[INFO_OFF_FLAGS]         = (uint8_t)(in.flags & 0x7Fu);
    return INFO_LEN;
}

// Read back a 32-bit field a build_info reply carries as five 7-bit bytes.
inline uint32_t info_u32(const uint8_t* payload, uint8_t offset)
{
    uint8_t raw[4] = {};
    if (sysex_codec::decode_7bit(payload + offset, raw, 5u, 4u) != 4u) return 0u;
    return ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16)
         | ((uint32_t)raw[2] << 8)  | (uint32_t)raw[3];
}

}  // namespace dfu_protocol
