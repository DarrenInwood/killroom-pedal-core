#pragma once
#include <cstdint>

// Parses MIDI from both transports — UART (DIN) and USB — through one parser
// per stream, and owns the routing of the DIN Out jack. What a message MEANS is
// the product's business: parsed events leave through six extern-C callbacks the
// product's main.cpp resolves at link time —
//
//   on_midi_cc(channel, cc, value)          on_midi_note_on(channel, note, vel)
//   on_midi_program_change(channel, prog)   on_midi_clock()
//   on_midi_sysex(data, len)                on_midi_clock_reset()
//
// A seventh, on_midi_note_off(channel, note, vel), is weak with an empty default: a
// product that tracks note lifetimes overrides it, and one that does not links without
// naming it. Both spellings of note off -- 0x80, and 0x90 with velocity 0 -- arrive there.
//
// A product with no use for notes or the MIDI-clock tempo layer defines those
// three as empty bodies; the parser still consumes the bytes, so the wire
// behaviour (including the DIN Out routing) is identical either way. SYSEX_RX_BUF
// comes from the product's pedal_core_ui_config.hpp.
//
// Every field of Config beyond `channel` and `omni` defaults to the behaviour a
// pedal had before it existed, so a product that never sets one is unchanged.
namespace midi_handler {

    // What the DIN Out jack carries.
    enum class OutMode : uint8_t {
        Merge = 0,   // inbound echo plus the pedal's own messages
        Thru  = 1,   // inbound echo only; the pedal stays silent on DIN
        Out   = 2,   // the pedal's own messages only; nothing is echoed
        Off   = 3,   // jack silent — the escape hatch for a MIDI loop
    };

    // Cross-routing between the two transports, which turns the pedal into a
    // MIDI interface: a host reaching downstream pedals, or a DIN controller
    // reaching the host.
    enum class UsbDinRoute : uint8_t {
        Off      = 0,
        UsbToDin = 1,
        DinToUsb = 2,
        Both     = 3,
    };

    // tx_channel's "follow the receive channel" value. An explicit 0-15 wins even
    // while omni is on, which is the whole point of separating the two: a pedal
    // listening to everything still has one channel it speaks on.
    inline constexpr uint8_t TX_CHANNEL_FOLLOW_RX = 0xFFu;

    struct Config {
        uint8_t     channel    = 0;      // 0-15, receive
        bool        omni       = false;  // receive only
        uint8_t     tx_channel = TX_CHANNEL_FOLLOW_RX;
        OutMode     out_mode   = OutMode::Merge;
        UsbDinRoute usb_din    = UsbDinRoute::Off;
        bool        clock_thru = true;   // forward an inbound clock, even in OutMode::Out
        bool        rx_pc      = true;   // act on Program Change
        bool        rx_sysex   = true;   // act on SysEx (see always_accepts_sysex below)
        bool        tx_params  = true;   // the pedal's own CC / NRPN echo leaves the pedal
    };

    void init(uint8_t channel, bool omni);
    void update();   // drain both transports; call every superloop wake

    void set_channel(uint8_t channel);
    void set_omni(bool omni);
    void set_config(const Config& cfg);
    const Config& get_config();

    // The channel the pedal speaks on: the configured transmit channel, or the
    // receive channel when it is set to follow.
    uint8_t tx_channel();

    // --- the DIN Out router ---------------------------------------------------
    //
    // Everything bound for the DIN jack goes through here so that two streams
    // (an inbound echo and the pedal's own traffic) cannot splice into each
    // other. The rule is the MIDI spec's: only System Real-Time bytes may appear
    // inside another message, so they are written straight through and every
    // other message is emitted whole.

    // One complete message the pedal originated (3-byte channel message, or a
    // whole F0..F7 frame). Dropped when out_mode does not carry the pedal's own
    // traffic. Contending with an inbound SysEx already in flight, it waits in a
    // small queue; a message that does not fit the queue is dropped rather than
    // spliced.
    void send_own(const uint8_t* msg, uint16_t len);

    // One System Real-Time byte the pedal originated — the generated MIDI clock.
    // Legal anywhere in the stream, so it is never queued.
    void send_own_realtime(uint8_t status);

    // A SysEx command the pedal answers even while rx_sysex is off, so the switch
    // can never lock an editor out: identity, the global-settings query, and
    // whatever command turns it back on. The product names the last of those.
    void set_sysex_always_accepted(const uint8_t* cmds, uint8_t count);

    // True while the pedal is generating its own MIDI clock. An inbound clock is
    // not forwarded while it is, whatever clock_thru says: exactly one clock
    // leaves the jack, which is an invariant rather than a setting a player has to
    // get right. midi_clock_out keeps this current.
    void set_generating_clock(bool on);
}
