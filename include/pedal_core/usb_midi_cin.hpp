#pragma once
#include <cstdint>

// USB-MIDI 1.0 Code Index Number (CIN) classification — the byte-count and
// CIN-selection logic that maps between USB-MIDI event packets and a raw MIDI
// byte stream.  Kept in this header (depends only on <cstdint>, NOT TinyUSB) so
// it is host-portable and can be unit-tested independently of the USB stack.
// usb_midi.cpp includes this and routes its packet RX/TX through these helpers.
namespace usb_midi_cin {

// RX: number of valid MIDI data bytes carried by a USB-MIDI event packet with
// the given CIN (low nibble of packet byte 0).  Per the USB-MIDI 1.0 table:
//   0x5 single-byte (System Common / SysEx-end-1) and 0xF single byte -> 1
//   0x2 (2-byte Sys Common), 0x6 (SysEx-end-2), 0xC (Program Change),
//       0xD (Channel Pressure)                                          -> 2
//   everything else (channel voice 0x8-0xB/0xE, 0x3, SysEx 0x4/0x7, ...) -> 3
inline uint8_t rx_data_count(uint8_t cin)
{
    switch (cin & 0x0F) {
        case 0x05: case 0x0F:                       return 1;
        case 0x02: case 0x06: case 0x0C: case 0x0D: return 2;
        default:                                    return 3;
    }
}

// TX: choose the CIN for an outgoing 1..3-byte MIDI message given its status
// byte and length.  1-byte: Tune Request (0xF6) is System Common (0x5), all
// other singletons are real-time Single Byte (0xF).  2-byte: Program Change
// (0xC0) and Channel Pressure (0xD0) are channel-voice and carry their own CIN
// in the status nibble; any other 2-byte message is System Common (0x2).
// 3-byte: channel-voice messages carry their CIN in the status nibble (Note
// 0x8/0x9, CC 0xB, PitchBend 0xE, ...); the lone 3-byte System Common — Song
// Position Pointer (0xF2) — is CIN 0x3, since its status nibble 0xF would
// otherwise be misread as a 1-byte Single Byte CIN.
inline uint8_t tx_cin(uint8_t status_byte, uint8_t len)
{
    const uint8_t status = status_byte & 0xF0;
    if (len == 1) return (status_byte == 0xF6) ? 0x05u : 0x0Fu;
    if (len == 2) return (status == 0xC0 || status == 0xD0)
                             ? (uint8_t)(status >> 4) : 0x02u;
    if (status == 0xF0) return 0x03u;  // len == 3 System Common (Song Position)
    return (uint8_t)(status >> 4);     // len == 3 channel voice
}

} // namespace usb_midi_cin
