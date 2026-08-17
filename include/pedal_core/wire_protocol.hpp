#pragma once
#include <cstdint>
#include "sysex_codec.hpp"

// The application's SysEx wire contract, shared by every pedal in the family
// and by the host editor. dfu_protocol.hpp covers the bootloader's half; this
// covers everything the running application says.
//
// Frames are  F0 <mfr> <device> <cmd> [payload] F7,  the same envelope the DFU
// commands use. The manufacturer byte is common to the family; each product
// picks its own device byte so two pedals can share a MIDI port.
//
// WHY THIS EXISTS
//
// Before protocol 3 the two pedals shared this envelope and their CC map and
// agreed on almost nothing else. DEVICE_INFO was sixteen bytes on one and
// fifteen on the other, because the multi-effect sent a bank count the distortion
// had no field for — so every byte after it shifted. The preset frame was 64
// bytes against 58, with the parameters starting at a different offset and the
// multi-effect carrying expression, sync and BPM fields in a tail the distortion
// did not have. GLOBAL_DATA differed again. A host had to carry two codecs and
// choose between them on the device byte, and a third pedal would have meant a
// third.
//
// Protocol 3 folds those into one layout. What a product does NOT have, it
// does not send:
//
//   - a capability mask in DEVICE_INFO says what the product supports, so a
//     host enables a panel from a bit rather than from the device byte;
//   - the variable parts of a frame ride as TLV records, which a reader skips
//     by length when it does not know the tag.
//
// That second property is the one worth protecting. A field added to one
// product does not move a byte for the other, and an editor built against
// protocol 3 keeps working against a firmware that has learned new tags.
//
// Command bytes and tags are APPEND-ONLY: an editor in the field speaks
// whatever version it shipped with, and DEVICE_INFO's version byte is how it
// finds out whether it can.
namespace pedal_core::wire {

inline constexpr uint8_t SYSEX_START = 0xF0u;
inline constexpr uint8_t SYSEX_END   = 0xF7u;

// Non-commercial / private-use manufacturer ID.
inline constexpr uint8_t MANUFACTURER_ID = 0x7Du;

// Bumped when a frame layout changes in a way an older host cannot parse.
// A host reads this from DEVICE_INFO and can refuse rather than misparse.
inline constexpr uint8_t PROTOCOL_VERSION = 3u;

// --- commands ---------------------------------------------------------------
// 0x01-0x0F belong to the DFU bootloader; see dfu_protocol.hpp.
namespace cmd {
inline constexpr uint8_t PRESET_DUMP_REQ  = 0x10u;  // no payload = live state; <bank><prog> = a stored slot
inline constexpr uint8_t PRESET_DUMP_DATA = 0x11u;
inline constexpr uint8_t PRESET_RESTORE   = 0x12u;  // a dump frame sent back at the pedal
inline constexpr uint8_t PRESET_SAVE      = 0x13u;  // <bank><prog> where SAVE_ADDRESSED is advertised
inline constexpr uint8_t SET_CHANNEL      = 0x20u;
inline constexpr uint8_t FETCH_GLOBAL     = 0x21u;
inline constexpr uint8_t GLOBAL_DATA      = 0x22u;
inline constexpr uint8_t SET_NOISE        = 0x23u;
inline constexpr uint8_t SET_EXT_INPUT    = 0x24u;
inline constexpr uint8_t SET_SYNC         = 0x25u;
inline constexpr uint8_t VERSION_REQ      = 0x30u;
inline constexpr uint8_t VERSION_DATA     = 0x31u;
inline constexpr uint8_t SET_BPM          = 0x40u;
inline constexpr uint8_t SET_NAME         = 0x41u;
inline constexpr uint8_t ENTER_CAL        = 0x50u;
inline constexpr uint8_t RESET_CAL        = 0x51u;
inline constexpr uint8_t LOAD_FACTORY     = 0x52u;
inline constexpr uint8_t DEVICE_INFO_REQ  = 0x70u;
inline constexpr uint8_t DEVICE_INFO_DATA = 0x71u;
inline constexpr uint8_t UID_REQ          = 0x72u;
inline constexpr uint8_t UID_DATA         = 0x73u;
}  // namespace cmd

// --- capabilities -----------------------------------------------------------
// Fourteen bits, carried as a lo7/hi7 pair in DEVICE_INFO. A host reads a
// capability rather than inferring one from the device byte, so a new product
// gets the panels it shares with an old one for free.
namespace cap {
inline constexpr uint16_t EXPRESSION     = 1u << 0;  // expression CCs and the preset's EXPR tag
inline constexpr uint16_t TEMPO          = 1u << 1;  // SET_BPM, SET_SYNC and the preset's TEMPO tag
inline constexpr uint16_t NOISE_GLOBAL   = 1u << 2;  // noise reduction is a global, not a preset parameter
inline constexpr uint16_t EXT_INPUT      = 1u << 3;
inline constexpr uint16_t BYPASS_FLAG    = 1u << 4;  // presets carry a bypassed-on-recall flag
inline constexpr uint16_t CALIBRATION    = 1u << 5;
inline constexpr uint16_t FACTORY_BANK   = 1u << 6;
inline constexpr uint16_t UID            = 1u << 7;  // answers UID_REQ
inline constexpr uint16_t SAVE_ADDRESSED = 1u << 8;  // PRESET_SAVE takes <bank><prog>
inline constexpr uint16_t BOOST          = 1u << 9;  // presets carry a second sound, the preset's BOOST tag
}  // namespace cap

// --- TLV tags ---------------------------------------------------------------
// A record is <tag> <len> <len bytes>, every byte 7-bit safe. An unknown tag is
// skipped by its length, which is what makes the layout extensible.
namespace preset_tag {
inline constexpr uint8_t EXPR  = 0x01u;  // <param 0x7F=off> <min_lo7> <min_hi7> <max_lo7> <max_hi7>
inline constexpr uint8_t TEMPO = 0x02u;  // <sync 0|1> <bpm_x10_lo7> <bpm_x10_hi7>
// The alternate sound a boost footswitch selects: a whole algorithm and parameter set,
// plus a signed step count for products that switch an input filter with it. Absent
// means the product has no second sound, or that it equals the primary.
inline constexpr uint8_t BOOST = 0x03u;  // <algorithm> <p0_lo7> <p0_hi7> ... <tighten+64>
}  // namespace preset_tag

namespace global_tag {
inline constexpr uint8_t CHANNEL   = 0x10u;  // 0-15, or 16 for omni
inline constexpr uint8_t NOISE     = 0x11u;  // <enabled> <threshold> <depth>
inline constexpr uint8_t EXT_INPUT = 0x12u;  // <mode> <tip> <ring> <both>
inline constexpr uint8_t BYPASS    = 0x13u;  // 0 bypassed, 1 active
}  // namespace global_tag

// The wire sentinel for "no expression assignment". The record keeps 0xFF,
// which is not a legal SysEx data byte, so the frame carries this instead.
inline constexpr uint8_t EXPR_OFF = 0x7Fu;

inline constexpr uint8_t MIDI_CHANNEL_OMNI = 16u;

// DEVICE_INFO_DATA is the one fixed-length frame, because a host has to parse
// it before it knows anything else about the pedal.
inline constexpr uint8_t DEVICE_INFO_FRAME_LEN = 17u;

// The MCU unique ID, 96 bits on every part in the family.
inline constexpr uint8_t UID_BYTE_LEN = 12u;

// The same ID 7-bit packed, as it travels: two groups of seven binary bytes
// become two MSB bytes plus twelve data bytes.
inline constexpr uint8_t UID_PACKED_LEN = 14u;

// --- addressing a pedal by unit ---------------------------------------------
//
// ENTER_BOOTLOADER carries the target's UID, and a pedal whose own ID does not
// match ignores it.
//
// This is not belt and braces. Over USB the command reaches one port and only
// one pedal hears it, but over DIN every pedal downstream of the sender sees
// every byte, and two pedals of the same model answer to the same device byte.
// An unaddressed reboot command would put an entire chain of them into DFU at
// once — and a bootloader cannot say which unit it is, so a host then cannot
// tell them apart to fix it. Each one needs a power cycle.
//
// A frame carrying no UID is therefore ignored rather than obeyed. The escape
// hatch for a pedal that cannot be addressed is the one that does not depend on
// firmware at all: hold both footswitches at power-on.
inline bool uid_matches(const uint8_t* payload, uint16_t plen,
                        const uint8_t* uid, uint8_t uid_len)
{
    if (payload == nullptr || uid == nullptr) return false;
    if (uid_len != UID_BYTE_LEN) return false;
    if (plen < UID_PACKED_LEN) return false;

    uint8_t decoded[UID_BYTE_LEN] = {};
    const uint16_t n = sysex_codec::decode_7bit(payload, decoded, UID_PACKED_LEN, sizeof(decoded));
    if (n != UID_BYTE_LEN) return false;

    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) {
        if (decoded[i] != uid[i]) return false;
    }
    return true;
}

// --- building ---------------------------------------------------------------

// An append-only writer over a caller-owned buffer.
//
// It never writes past the end and never reports success after it has had to
// stop, so a frame that outgrows its buffer is dropped rather than truncated
// and sent — a half preset frame would restore as garbage.
class Writer {
public:
    constexpr Writer(uint8_t* buf, uint16_t cap) : m_buf(buf), m_cap(cap) {}

    constexpr void u7(uint8_t v)
    {
        if (m_len >= m_cap) { m_ok = false; return; }
        m_buf[m_len++] = (uint8_t)(v & 0x7Fu);
    }

    // A value as the lo7/hi7 pair the frames carry it as.
    constexpr void u14(uint16_t v)
    {
        u7((uint8_t)(v & 0x7Fu));
        u7((uint8_t)((v >> 7) & 0x7Fu));
    }

    constexpr void bytes(const uint8_t* src, uint16_t n)
    {
        for (uint16_t i = 0; i < n; ++i) u7(src[i]);
    }

    // Open a frame: F0 <mfr> <device> <cmd>.
    constexpr void header(uint8_t device, uint8_t command)
    {
        if (m_len >= m_cap) { m_ok = false; return; }
        m_buf[m_len++] = SYSEX_START;   // 0xF0 is a status byte, so not through u7
        u7(MANUFACTURER_ID);
        u7(device);
        u7(command);
    }

    constexpr void end()
    {
        if (m_len >= m_cap) { m_ok = false; return; }
        m_buf[m_len++] = SYSEX_END;
    }

    // A whole TLV record.
    constexpr void tlv(uint8_t tag, const uint8_t* value, uint8_t n)
    {
        u7(tag);
        u7(n);
        bytes(value, n);
    }

    constexpr uint16_t length() const { return m_ok ? m_len : 0u; }
    constexpr bool     ok() const     { return m_ok; }

private:
    uint8_t* m_buf;
    uint16_t m_cap;
    uint16_t m_len = 0u;
    bool     m_ok  = true;
};

// What DEVICE_INFO reports. Everything a host needs to size its own buffers and
// decide which panels to show, and nothing it can work out for itself — the
// bank count went in protocol 3 because it is ceil(slots / 128).
struct DeviceInfo {
    uint8_t  device_id;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint16_t slots;
    uint8_t  algorithm_count;   // effects, distortion algorithms — whatever the selector picks
    uint8_t  param_count;
    uint8_t  param_bits;
    uint8_t  name_len;
    uint16_t capabilities;
};

// Returns the frame length, or 0 if the buffer was too small.
inline uint16_t build_device_info(const DeviceInfo& d, uint8_t* out, uint16_t cap)
{
    Writer w(out, cap);
    w.header(d.device_id, cmd::DEVICE_INFO_DATA);
    w.u7(PROTOCOL_VERSION);
    w.u7(d.fw_major);
    w.u7(d.fw_minor);
    w.u7(d.fw_patch);
    w.u14(d.slots);
    w.u7(d.algorithm_count);
    w.u7(d.param_count);
    w.u7(d.param_bits);
    w.u7(d.name_len);
    w.u14(d.capabilities);
    w.end();
    return w.length();
}

// UID_DATA: the MCU's 96-bit unique ID, 7-bit packed by the caller (see
// sysex_codec.hpp) because the packing lives there.
inline uint16_t build_uid(uint8_t device, const uint8_t* packed, uint8_t packed_len,
                          uint8_t* out, uint16_t cap)
{
    Writer w(out, cap);
    w.header(device, cmd::UID_DATA);
    w.u7(PROTOCOL_VERSION);
    w.bytes(packed, packed_len);
    w.end();
    return w.length();
}

// --- reading ----------------------------------------------------------------

struct Tlv {
    uint8_t        tag;
    const uint8_t* value;
    uint8_t        len;
};

// Walks a TLV run.
//
// A truncated record ends the walk rather than failing the frame: a short tail
// is what a dropped packet looks like, and the records already read are still
// good. The caller decides whether the fields it needed turned up.
class TlvReader {
public:
    constexpr TlvReader(const uint8_t* buf, uint16_t len) : m_buf(buf), m_len(len) {}

    constexpr bool next(Tlv& out)
    {
        if ((uint16_t)(m_pos + 2u) > m_len) return false;
        const uint8_t tag = m_buf[m_pos];
        const uint8_t n   = m_buf[m_pos + 1u];
        if ((uint16_t)(m_pos + 2u + n) > m_len) return false;   // truncated
        out.tag   = tag;
        out.value = &m_buf[m_pos + 2u];
        out.len   = n;
        m_pos = (uint16_t)(m_pos + 2u + n);
        return true;
    }

    // The first record with this tag, or false. Restarts the walk, so it is
    // fine to call for each field a handler wants.
    constexpr bool find(uint8_t tag, Tlv& out)
    {
        m_pos = 0u;
        Tlv t{};
        while (next(t)) {
            if (t.tag == tag) { out = t; return true; }
        }
        return false;
    }

private:
    const uint8_t* m_buf;
    uint16_t       m_len;
    uint16_t       m_pos = 0u;
};

// The lo7/hi7 pair the frames carry a value as.
constexpr uint16_t read_u14(const uint8_t* p)
{
    return (uint16_t)((uint16_t)(p[0] & 0x7Fu) | (uint16_t)((uint16_t)(p[1] & 0x7Fu) << 7));
}

// --- the preset frame -------------------------------------------------------
//
//   F0 <mfr> <dev> <cmd>
//     <bank> <program> <format> <algorithm> <flags> <param_count>
//     <p0_lo7> <p0_hi7> ... <name_len> <name bytes> <TLV...> F7
//
// The head is fixed so a reader can find the parameters without knowing the
// product; everything that varies between products rides in the tail. Shared by
// PRESET_DUMP_DATA and PRESET_RESTORE, which differ only in direction.

inline constexpr uint8_t PRESET_HEAD_LEN = 6u;   // bank..param_count

// The largest preset frame a product can produce, so it can size its buffer
// once from its own constants rather than guessing and hoping.
constexpr uint16_t preset_frame_max(uint8_t param_count, uint8_t name_len, uint8_t tlv_bytes)
{
    return (uint16_t)(5u                                  // F0 mfr dev cmd ... F7
                      + PRESET_HEAD_LEN
                      + (uint16_t)param_count * 2u
                      + 1u + (uint16_t)name_len           // name length byte, then the name
                      + (uint16_t)tlv_bytes);
}

struct PresetHead {
    uint16_t slot;
    uint8_t  format;        // PROTOCOL_VERSION at the time it was written
    uint8_t  algorithm;     // stable ID, never a menu position
    uint8_t  flags;         // bit 0: bypassed on recall
    uint8_t  param_count;
};

inline void write_preset_head(Writer& w, uint8_t device, uint8_t command, const PresetHead& h)
{
    w.header(device, command);
    w.u7((uint8_t)((h.slot >> 7) & 0x7Fu));   // bank
    w.u7((uint8_t)(h.slot & 0x7Fu));          // program
    w.u7(h.format);
    w.u7(h.algorithm);
    w.u7(h.flags);
    w.u7(h.param_count);
}

// A parsed frame, pointing into the caller's buffer rather than copying it.
struct PresetView {
    PresetHead     head;
    const uint8_t* params;    // param_count lo7/hi7 pairs
    const uint8_t* name;
    uint8_t        name_len;
    const uint8_t* tlv;       // may be empty
    uint16_t       tlv_len;
};

// Decode a preset frame. Returns false for anything that does not add up —
// a foreign manufacturer, the wrong device, a parameter block or name that
// runs off the end. A frame from an editor is as untrusted as one off a
// corrupt page, so the bounds are checked before any of it is believed.
inline bool parse_preset(const uint8_t* f, uint16_t len, uint8_t device, PresetView& out)
{
    if (f == nullptr || len < (uint16_t)(5u + PRESET_HEAD_LEN)) return false;
    if (f[0] != SYSEX_START || f[1] != MANUFACTURER_ID || f[2] != device) return false;
    if (f[len - 1u] != SYSEX_END) return false;
    if (f[3] != cmd::PRESET_DUMP_DATA && f[3] != cmd::PRESET_RESTORE) return false;

    const uint16_t end = (uint16_t)(len - 1u);   // index of F7
    uint16_t i = 4u;

    out.head.slot        = (uint16_t)((uint16_t)f[i] * 128u + f[i + 1u]);
    out.head.format      = f[i + 2u];
    out.head.algorithm   = f[i + 3u];
    out.head.flags       = f[i + 4u];
    out.head.param_count = f[i + 5u];
    i = (uint16_t)(i + PRESET_HEAD_LEN);

    const uint16_t param_bytes = (uint16_t)((uint16_t)out.head.param_count * 2u);
    if ((uint16_t)(i + param_bytes) > end) return false;
    out.params = &f[i];
    i = (uint16_t)(i + param_bytes);

    if (i >= end) return false;
    out.name_len = f[i++];
    if ((uint16_t)(i + out.name_len) > end) return false;
    out.name = &f[i];
    i = (uint16_t)(i + out.name_len);

    out.tlv     = &f[i];
    out.tlv_len = (uint16_t)(end - i);
    return true;
}

}  // namespace pedal_core::wire
