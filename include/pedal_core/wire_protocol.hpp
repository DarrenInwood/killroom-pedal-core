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
// Change what the running preset says about its Play knobs, its scene or its
// footswitches, without touching anything else it holds. The payload is a TLV list
// using the preset frame's own MACROS / SCENE / SWITCHES tags, so a fourth extra
// needs a tag rather than another command. A tag the frame omits is left alone; a
// zero-length tag clears that field back to "the algorithm's answer".
//
// Live state, like SET_NAME and SET_BPM: it takes effect at once and a save is what
// persists it. A restore writes storage instead, and cannot reach the running sound.
inline constexpr uint8_t SET_PRESET_EXTRAS = 0x14u;
// Put the pedal on one side of the preset's A/B, the way a footswitch set to Scene
// A/B does: <0|1>, 0 = Scene A. Absolute rather than a toggle, so a host that has
// just read SCENE_ACTIVE can ask for a known side instead of guessing which way the
// latch points. A preset with no scene has nothing to jump to and ignores it.
inline constexpr uint8_t SET_SCENE_ACTIVE = 0x15u;
inline constexpr uint8_t SET_CHANNEL      = 0x20u;
inline constexpr uint8_t FETCH_GLOBAL     = 0x21u;
inline constexpr uint8_t GLOBAL_DATA      = 0x22u;
inline constexpr uint8_t SET_NOISE        = 0x23u;
inline constexpr uint8_t SET_EXT_INPUT    = 0x24u;
inline constexpr uint8_t SET_SYNC         = 0x25u;
inline constexpr uint8_t SET_MIDI         = 0x26u;  // the whole routing block; see midi_routing
// How the pedal's knobs behave when a parameter has moved out from under them:
// 0 pickup, 1 jump. A device preference, so it belongs beside the other globals
// rather than in a preset. A device without knobs never answers it.
inline constexpr uint8_t SET_KNOB_MODE    = 0x27u;
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
inline constexpr uint16_t MIDI_ROUTING   = 1u << 10; // SET_MIDI and the global MIDI_ROUTING tag
inline constexpr uint16_t PRESET_EXTRAS  = 1u << 11; // SET_PRESET_EXTRAS and the MACROS/SCENE/SWITCHES tags
// The pedal reports which side of the A/B is sounding and can be put on either: the
// SCENE_ACTIVE global tag, SET_SCENE_ACTIVE and CC 16. Separate from PRESET_EXTRAS
// because a product can carry a scene and jump under a footswitch without being able
// to say which half you are hearing, and a host must not guess at that.
inline constexpr uint16_t SCENE_LATCH    = 1u << 12;
// The TEMPO tag's note-division bytes, and the SET_PRESET_EXTRAS tag that writes them.
// Separate from TEMPO because a product can sync to a clock without letting the player
// choose which note value the tempo drives. This is the last bit the field holds: the
// mask is carried lo7/hi7, so a fifteenth capability needs DEVICE_INFO's field widened.
inline constexpr uint16_t TEMPO_DIVISION = 1u << 13;
}  // namespace cap

// --- TLV tags ---------------------------------------------------------------
// A record is <tag> <len> <len bytes>, every byte 7-bit safe. An unknown tag is
// skipped by its length, which is what makes the layout extensible.
namespace preset_tag {
inline constexpr uint8_t EXPR  = 0x01u;  // <param 0x7F=off> <min_lo7> <min_hi7> <max_lo7> <max_hi7>
// The tempo. bpm_x10 is what the preset sounds like and is always present; the two bytes
// after it say how that tempo is built — which note value one repeat or one LFO cycle
// occupies, and how many octaves the parameter's range had to move that note to reach it
// (biased by 4, so a stored 4 is a fold of 0). A reader that predates them takes the three
// bytes it knows and skips the rest, and a writer that omits them is read as the plain
// beat, so the record grew without a protocol version bump. The fold is ignored inbound:
// it is a consequence of the note and the tempo, and the device recomputes it.
inline constexpr uint8_t TEMPO = 0x02u;  // <sync 0|1> <bpm_lo7> <bpm_hi7> [<div> <fold+4>]
inline constexpr uint8_t TEMPO_FOLD_BIAS = 4u;
// The alternate sound a boost footswitch selects: a whole algorithm and parameter set,
// plus a signed step count for products that switch an input filter with it. Absent
// means the product has no second sound, or that it equals the primary.
inline constexpr uint8_t BOOST = 0x03u;  // <algorithm> <p0_lo7> <p0_hi7> ... <tighten+64>
// The four controls a preset puts under the knobs in performance, as parameter indices.
// UNSET leaves one to the algorithm's own choice, which is what a preset that has never
// been told otherwise says. Absent means the same for all four.
inline constexpr uint8_t MACROS   = 0x04u;  // <m0> <m1> <m2> <m3>
// A second value for each of those four: the sound a scene switch jumps to. Absent means
// the preset has no scene, which is not the same as a scene whose values happen to be zero.
inline constexpr uint8_t SCENE    = 0x05u;  // <s0_lo7> <s0_hi7> ... <s3_lo7> <s3_hi7>
// What this preset's own footswitches do, where it wants something other than the
// algorithm's answer: press and hold for each of the two. UNSET is the algorithm's answer.
inline constexpr uint8_t SWITCHES = 0x06u;  // <fs1_press> <fs1_hold> <fs2_press> <fs2_hold>

// "Not set" on the wire. The record's own sentinels differ per field (0xFF for a macro,
// 0x0F for a switch) and neither is worth exporting: one is not a legal SysEx data byte at
// all, and the other is a value a longer action list could one day reach.
inline constexpr uint8_t UNSET = 0x7Fu;
}  // namespace preset_tag

namespace global_tag {
inline constexpr uint8_t CHANNEL   = 0x10u;  // 0-15, or 16 for omni
inline constexpr uint8_t NOISE     = 0x11u;  // <enabled> <threshold> <depth>
// The jack's mode and what each of its three contacts does. Each contact carries a press
// and a hold, and the three hold assignments are the optional tail: a device that predates
// them sends four bytes, and a host that does reads seven. The record is length-prefixed,
// so growing it needed no version bump on either side.
inline constexpr uint8_t EXT_INPUT = 0x12u;  // <mode> <tip> <ring> <both> [<t_hold> <r_hold> <b_hold>]
inline constexpr uint8_t BYPASS    = 0x13u;  // 0 bypassed, 1 active
// The MIDI routing block, midi_routing::LEN bytes in the order that namespace
// gives. CHANNEL stays alongside it, carrying the same receive channel in the
// older one-byte form, so a host that predates this tag still reads a channel.
inline constexpr uint8_t MIDI_ROUTING = 0x14u;
// Which side of the running preset's A/B is sounding: 0 Scene A, 1 Scene B. Read
// rather than announced, like the rest of this frame — a host that polls the globals
// sees a player's stomp on its next read.
inline constexpr uint8_t SCENE_ACTIVE = 0x15u;
// How the knobs behave when a parameter has moved out from under them: 0 pickup — the
// knob stays inert until its pot reaches the value — 1 jump, the knob is always live.
// A host that predates this tag skips it by its length and reads the rest of the frame.
//
// No capability bit announces it, and none can: cap:: is full at fourteen, and a fifteenth
// needs DEVICE_INFO's field widened, which is a wire change rather than a constant. Nor
// does it need one -- a bit exists for what a global read cannot show, and this record
// either is in the frame or is not, which is the whole point of the layout being
// length-prefixed. A product with no knobs sends no record and a host reads no knob mode.
inline constexpr uint8_t KNOB_MODE    = 0x16u;

// The newest tag in this namespace, and the reason a new one cannot be half-added.
// GLOBAL_RECORDS below must reach it: a tag named here with no row there is a tag the
// firmware can spell but cannot send, and a host reading the frame skips it by its length
// and never learns it was meant to exist. Move this line with the tag above it.
inline constexpr uint8_t LAST = KNOB_MODE;
}  // namespace global_tag

// --- the MIDI routing block -------------------------------------------------
// One payload shared by SET_MIDI and the MIDI_ROUTING tag, so a host writes back
// exactly what it read. Every byte is 7-bit safe; a value outside its range is
// the pedal's to clamp, not the host's to assume.
namespace midi_routing {

// What the MIDI Out jack carries.
namespace out_mode {
inline constexpr uint8_t MERGE = 0u;  // inbound echo plus the pedal's own messages
inline constexpr uint8_t THRU  = 1u;  // inbound echo only
inline constexpr uint8_t OUT   = 2u;  // the pedal's own messages only
inline constexpr uint8_t OFF   = 3u;  // silent
inline constexpr uint8_t COUNT = 4u;
}

// Cross-routing between the transports, which makes the pedal a MIDI interface.
namespace usb_jack {
inline constexpr uint8_t OFF        = 0u;
inline constexpr uint8_t USB_TO_JACK = 1u;
inline constexpr uint8_t JACK_TO_USB = 2u;
inline constexpr uint8_t BOTH       = 3u;
inline constexpr uint8_t COUNT      = 4u;
}

// Which of its own state changes the pedal announces. A pedal announces only a
// change a player made on it -- never one a host asked for -- so Out patched
// back to In cannot loop.
namespace tx_state {
inline constexpr uint8_t OFF        = 0u;
inline constexpr uint8_t PC         = 1u;  // Bank Select + Program Change on preset change
inline constexpr uint8_t BYPASS     = 2u;  // the bypass CC on a footswitch toggle
inline constexpr uint8_t PC_BYPASS  = 3u;  // both
inline constexpr uint8_t COUNT      = 4u;
}

// The transmit channel's "follow the receive channel" value. 0xFF is not a legal
// SysEx data byte, so the block carries this instead -- the same trick EXPR_OFF
// plays for the expression assignment.
inline constexpr uint8_t TX_CHANNEL_FOLLOW_RX = 0x7Fu;

// Payload byte offsets.
inline constexpr uint8_t RX_CHANNEL = 0u;   // 0-15
inline constexpr uint8_t OMNI       = 1u;   // 0 or 1
inline constexpr uint8_t TX_CHANNEL = 2u;   // 0-15, or TX_CHANNEL_FOLLOW_RX
inline constexpr uint8_t OUT_MODE   = 3u;   // out_mode::*
inline constexpr uint8_t PC_OFFSET  = 4u;   // 0-127; the program that addresses slot 0
inline constexpr uint8_t CLOCK_OUT  = 5u;   // 0 or 1
inline constexpr uint8_t CLOCK_THRU = 6u;   // 0 or 1
inline constexpr uint8_t USB_JACK    = 7u;   // usb_jack::*
inline constexpr uint8_t RX_PC      = 8u;   // 0 or 1
inline constexpr uint8_t RX_SYSEX   = 9u;   // 0 or 1
inline constexpr uint8_t TX_PARAMS  = 10u;  // 0 or 1
inline constexpr uint8_t TX_STATE   = 11u;  // tx_state::*
inline constexpr uint8_t LEN        = 12u;

// The block as a value, in the wire's own vocabulary: every field is the byte the
// payload carries, so a sentinel here is the wire's sentinel and a mode here is one of
// the constants above. What a pedal does with it is midi_routing.hpp's business.
//
// The defaults are the behaviour a pedal has with no block stored, so a default value
// round-trips to a default configuration rather than to a silent jack.
struct RoutingBlock {
    uint8_t rx_channel = 0u;                       // 0-15
    bool    omni       = false;
    uint8_t tx_channel = TX_CHANNEL_FOLLOW_RX;     // 0-15, or the follow sentinel
    uint8_t out        = out_mode::MERGE;
    uint8_t pc_offset  = 0u;                       // 0-127
    bool    clock_out  = false;
    bool    clock_thru = true;
    uint8_t usb_jack_route = usb_jack::OFF;
    bool    rx_pc      = true;
    bool    rx_sysex   = true;
    bool    tx_params  = true;
    uint8_t tx         = tx_state::OFF;
};

// Read the block out of a LEN-byte payload. Values are taken as sent: a byte outside
// its range is the pedal's to clamp, which is what applying the block does.
inline void read_block(const uint8_t* p, RoutingBlock& out)
{
    out.rx_channel    = (uint8_t)(p[RX_CHANNEL] & 0x7Fu);
    out.omni          = (p[OMNI] != 0u);
    out.tx_channel    = (uint8_t)(p[TX_CHANNEL] & 0x7Fu);
    out.out           = (uint8_t)(p[OUT_MODE] & 0x7Fu);
    out.pc_offset     = (uint8_t)(p[PC_OFFSET] & 0x7Fu);
    out.clock_out     = (p[CLOCK_OUT] != 0u);
    out.clock_thru    = (p[CLOCK_THRU] != 0u);
    out.usb_jack_route = (uint8_t)(p[USB_JACK] & 0x7Fu);
    out.rx_pc         = (p[RX_PC] != 0u);
    out.rx_sysex      = (p[RX_SYSEX] != 0u);
    out.tx_params     = (p[TX_PARAMS] != 0u);
    out.tx            = (uint8_t)(p[TX_STATE] & 0x7Fu);
}

// Lay the block into a LEN-byte payload, in the order this namespace gives, so a host
// writes back exactly what it read.
inline void write_block(const RoutingBlock& b, uint8_t* out)
{
    out[RX_CHANNEL] = (uint8_t)(b.rx_channel & 0x7Fu);
    out[OMNI]       = b.omni ? 1u : 0u;
    out[TX_CHANNEL] = (uint8_t)(b.tx_channel & 0x7Fu);
    out[OUT_MODE]   = (uint8_t)(b.out & 0x7Fu);
    out[PC_OFFSET]  = (uint8_t)(b.pc_offset & 0x7Fu);
    out[CLOCK_OUT]  = b.clock_out ? 1u : 0u;
    out[CLOCK_THRU] = b.clock_thru ? 1u : 0u;
    out[USB_JACK]    = (uint8_t)(b.usb_jack_route & 0x7Fu);
    out[RX_PC]      = b.rx_pc ? 1u : 0u;
    out[RX_SYSEX]   = b.rx_sysex ? 1u : 0u;
    out[TX_PARAMS]  = b.tx_params ? 1u : 0u;
    out[TX_STATE]   = (uint8_t)(b.tx & 0x7Fu);
}

}  // namespace midi_routing

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
// one pedal hears it, but over the jack every pedal downstream of the sender sees
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


// --- the global frame -------------------------------------------------------
//
//   F0 <mfr> <dev> <cmd> <TLV...> F7
//
// Everything the pedal holds that is not part of a preset. Unlike the preset frame there
// is no fixed head at all: every field rides as a TLV record, so a product sends only
// what it has, and a reader skips a tag it does not know by length.
//
// Each field therefore carries whether it was present. A tag the frame omits is not a
// zero — it is a question the pedal was not asked, and applying it as a value would turn
// a capability the product lacks into a setting it appears to have turned off.

struct NoiseSettings {
    bool    enabled   = false;
    uint8_t threshold = 0u;
    uint8_t depth     = 0u;
};

// The jack's mode, and the action each of its three contacts carries. The hold
// assignments are the optional tail: a device that predates them sends four bytes.
struct ExtInputSettings {
    uint8_t mode     = 0u;
    uint8_t press[3] = {};      // tip, ring, both
    uint8_t hold[3]  = {};
    bool    has_holds = false;
};

// --- the global block's records ---------------------------------------------
//
// Every record the frame carries, stated once: its tag, the shortest payload worth
// believing, and the longest it is ever written with. The size bound and the decoder's
// length check are both answered from here, so the three cannot drift apart by hand.
//
// The encoder and the decoder keep their own branches deliberately. Each record packs a
// different shape -- a bool, a three-byte triple, a block with an optional tail -- and a
// table that could express all of them would need offsets into GlobalView, which is not a
// standard-layout type and is compiled in every product under -Wall -Wextra. What the
// table is for is making an omission a compile error, not making the codec generic.
struct GlobalRecord {
    uint8_t tag;
    uint8_t min_len;   // a record shorter than this names a field it cannot fill
    uint8_t max_len;   // what the frame bound budgets for
};

inline constexpr GlobalRecord GLOBAL_RECORDS[] = {
    { global_tag::CHANNEL,      1u,                 1u                 },
    { global_tag::NOISE,        3u,                 3u                 },
    { global_tag::EXT_INPUT,    4u,                 7u                 },  // the holds are the tail
    { global_tag::BYPASS,       1u,                 1u                 },
    { global_tag::MIDI_ROUTING, midi_routing::LEN,  midi_routing::LEN  },
    { global_tag::SCENE_ACTIVE, 1u,                 1u                 },
    { global_tag::KNOB_MODE,    1u,                 1u                 },
};

inline constexpr uint16_t GLOBAL_RECORD_COUNT =
    (uint16_t)(sizeof(GLOBAL_RECORDS) / sizeof(GLOBAL_RECORDS[0]));

// The tags are one run allocated from CHANNEL upward. Holding the table to that run, and
// to both ends of it, is what makes "every tag has a row" checkable without reflection:
// a tag added to the namespace moves global_tag::LAST, and the last assertion then fails
// until the row exists.
inline constexpr bool global_records_run_unbroken()
{
    for (uint16_t i = 1u; i < GLOBAL_RECORD_COUNT; ++i)
        if (GLOBAL_RECORDS[i].tag != (uint8_t)(GLOBAL_RECORDS[i - 1u].tag + 1u)) return false;
    return true;
}
static_assert(GLOBAL_RECORDS[0].tag == global_tag::CHANNEL,
              "GLOBAL_RECORDS starts at the first global tag");
static_assert(global_records_run_unbroken(),
              "the global tags are one unbroken run; GLOBAL_RECORDS must be in that order");
static_assert(GLOBAL_RECORDS[GLOBAL_RECORD_COUNT - 1u].tag == global_tag::LAST,
              "a tag was added to global_tag:: without a row in GLOBAL_RECORDS");

// The shortest payload this tag is worth reading. Zero for a tag this firmware does not
// know, which is exactly the length check an unknown record should get: none, because it
// is skipped by its length either way.
inline constexpr uint8_t global_min_len(uint8_t tag)
{
    for (const GlobalRecord& r : GLOBAL_RECORDS)
        if (r.tag == tag) return r.min_len;
    return 0u;
}

struct GlobalView {
    uint8_t                    channel       = 0u;     // 0-15, or MIDI_CHANNEL_OMNI
    NoiseSettings              noise{};
    ExtInputSettings           ext_input{};
    bool                       bypass_active = true;
    midi_routing::RoutingBlock routing{};
    uint8_t                    scene_active  = 0u;     // 0 Scene A, 1 Scene B
    uint8_t                    knob_mode     = 0u;     // 0 pickup, 1 jump

    bool has_channel      = false;
    bool has_noise        = false;
    bool has_ext_input    = false;
    bool has_bypass       = false;
    bool has_routing      = false;
    bool has_scene_active = false;
    bool has_knob_mode    = false;
};

// The largest global frame a product can produce, so it can size its buffer once from a
// constant rather than guessing. Every record at its longest, plus the tag and length byte
// each costs -- summed from the table, so a record added there is budgeted for without
// anyone remembering to add a term here.
inline constexpr uint16_t global_frame_max()
{
    uint16_t n = 5u;   // F0 mfr dev cmd ... F7
    for (const GlobalRecord& r : GLOBAL_RECORDS) n = (uint16_t)(n + 2u + r.max_len);
    return n;
}
inline constexpr uint16_t GLOBAL_FRAME_MAX = global_frame_max();

// Returns the frame length, or 0 if the buffer was too small. A record whose field was
// not present is not written: what a product does not have, it does not send.
inline uint16_t build_global(const GlobalView& g, uint8_t device, uint8_t* out, uint16_t cap)
{
    Writer w(out, cap);
    w.header(device, cmd::GLOBAL_DATA);

    if (g.has_channel) {
        const uint8_t v = g.channel;
        w.tlv(global_tag::CHANNEL, &v, 1u);
    }
    if (g.has_noise) {
        const uint8_t v[3] = { (uint8_t)(g.noise.enabled ? 1u : 0u),
                               g.noise.threshold, g.noise.depth };
        w.tlv(global_tag::NOISE, v, 3u);
    }
    if (g.has_ext_input) {
        const uint8_t v[7] = { g.ext_input.mode,
                               g.ext_input.press[0], g.ext_input.press[1], g.ext_input.press[2],
                               g.ext_input.hold[0],  g.ext_input.hold[1],  g.ext_input.hold[2] };
        w.tlv(global_tag::EXT_INPUT, v, g.ext_input.has_holds ? 7u : 4u);
    }
    if (g.has_bypass) {
        const uint8_t v = g.bypass_active ? 1u : 0u;
        w.tlv(global_tag::BYPASS, &v, 1u);
    }
    if (g.has_routing) {
        uint8_t v[midi_routing::LEN] = {};
        midi_routing::write_block(g.routing, v);
        w.tlv(global_tag::MIDI_ROUTING, v, midi_routing::LEN);
    }
    if (g.has_scene_active) {
        const uint8_t v = g.scene_active;
        w.tlv(global_tag::SCENE_ACTIVE, &v, 1u);
    }
    if (g.has_knob_mode) {
        const uint8_t v = g.knob_mode;
        w.tlv(global_tag::KNOB_MODE, &v, 1u);
    }

    w.end();
    return w.length();
}

// Decode a global frame. Returns false for anything that does not add up — a foreign
// manufacturer, the wrong device, the wrong command. A record shorter than the field it
// names is left absent rather than half-believed: a frame from an editor is as untrusted
// as one off a corrupt page.
inline bool parse_global(const uint8_t* f, uint16_t len, uint8_t device, GlobalView& out)
{
    if (f == nullptr || len < 5u) return false;
    if (f[0] != SYSEX_START || f[1] != MANUFACTURER_ID || f[2] != device) return false;
    if (f[len - 1u] != SYSEX_END) return false;
    if (f[3] != cmd::GLOBAL_DATA) return false;

    out = GlobalView{};

    TlvReader r(&f[4], (uint16_t)(len - 5u));
    Tlv t{};
    while (r.next(t)) {
        // A record shorter than the field it names is left absent rather than half-believed:
        // a frame from an editor is as untrusted as one off a corrupt page. The bound comes
        // from the table the encoder is budgeted from, so the two cannot disagree about how
        // long a record has to be. An unknown tag has no bound and is skipped below.
        if (t.len < global_min_len(t.tag)) continue;

        switch (t.tag) {
            case global_tag::CHANNEL:
                out.channel     = t.value[0];
                out.has_channel = true;
                break;
            case global_tag::NOISE:
                out.noise.enabled   = (t.value[0] != 0u);
                out.noise.threshold = t.value[1];
                out.noise.depth     = t.value[2];
                out.has_noise       = true;
                break;
            case global_tag::EXT_INPUT:
                out.ext_input.mode     = t.value[0];
                out.ext_input.press[0] = t.value[1];
                out.ext_input.press[1] = t.value[2];
                out.ext_input.press[2] = t.value[3];
                if (t.len >= 7u) {
                    out.ext_input.hold[0]   = t.value[4];
                    out.ext_input.hold[1]   = t.value[5];
                    out.ext_input.hold[2]   = t.value[6];
                    out.ext_input.has_holds = true;
                }
                out.has_ext_input = true;
                break;
            case global_tag::BYPASS:
                out.bypass_active = (t.value[0] != 0u);
                out.has_bypass    = true;
                break;
            case global_tag::MIDI_ROUTING:
                midi_routing::read_block(t.value, out.routing);
                out.has_routing = true;
                break;
            case global_tag::SCENE_ACTIVE:
                out.scene_active     = t.value[0];
                out.has_scene_active = true;
                break;
            case global_tag::KNOB_MODE:
                out.knob_mode     = t.value[0];
                out.has_knob_mode = true;
                break;
            default:
                break;   // a tag this firmware does not know, skipped by length
        }
    }
    return true;
}

}  // namespace pedal_core::wire
