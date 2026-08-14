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
//   Enter DFU (sent to the application):
//     F0 <mfr> <dev> 01 F7
//   Begin firmware — the total image size, so the bootloader can show a
//   percentage. Also resets the write session, so resending it starts a clean
//   retry after a failure:
//     F0 <mfr> <dev> 05 [size4, 7-bit packed as 5 bytes] F7
//   Firmware chunk:
//     F0 <mfr> <dev> 02 [addr3 addr2 addr1 addr0] [len_hi len_lo]
//                       [7-bit packed data] F7
//   End of firmware — CRC32 over the whole image:
//     F0 <mfr> <dev> 03 [crc4, 7-bit packed] F7
//   Status, from the bootloader after every command:
//     F0 <mfr> <dev> 7E [status] [addr3 addr2 addr1 addr0] F7
namespace dfu_protocol {

inline constexpr uint8_t CMD_ENTER_BOOTLOADER = 0x01u;
inline constexpr uint8_t CMD_FW_CHUNK         = 0x02u;
inline constexpr uint8_t CMD_FW_END           = 0x03u;
inline constexpr uint8_t CMD_FW_STATUS        = 0x7Eu;
inline constexpr uint8_t CMD_FW_BEGIN         = 0x05u;

enum class Status : uint8_t {
    Ack      = 0x00u,   // chunk accepted and written
    Nack     = 0x01u,   // malformed, out of range, or a failed flash write
    CrcFail  = 0x02u,   // the image arrived but its CRC32 does not match
    Complete = 0x03u,   // verified; the bootloader is about to reboot
};

// The address a status frame reports back: for a chunk, where the next byte is
// expected — which is what lets a host resume rather than restart.
inline constexpr uint8_t STATUS_FRAME_LEN = 11u;   // F0 mfr dev 7E st a3 a2 a1 a0 .. F7

}  // namespace dfu_protocol
