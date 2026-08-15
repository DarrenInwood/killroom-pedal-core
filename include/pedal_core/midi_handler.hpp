#pragma once
#include <cstdint>

// Parses MIDI from both transports — UART (DIN) and USB — through one parser
// per stream, and owns the byte-level DIN Thru. What a message MEANS is the
// product's business: parsed events leave through six extern-C callbacks the
// product's main.cpp resolves at link time —
//
//   on_midi_cc(channel, cc, value)          on_midi_note_on(channel, note, vel)
//   on_midi_program_change(channel, prog)   on_midi_clock()
//   on_midi_sysex(data, len)                on_midi_clock_reset()
//
// A product with no use for notes or the MIDI-clock tempo layer defines those
// three as empty bodies; the parser still consumes the bytes, so the wire
// behaviour (including Thru) is identical either way. SYSEX_RX_BUF comes from
// the product's pedal_core_ui_config.hpp.
namespace midi_handler {

    struct Config {
        uint8_t channel;   // 0-15
        bool    omni;
    };

    void init(uint8_t channel, bool omni);
    void update();   // drain both transports; call every superloop wake
    void set_channel(uint8_t channel);
    void set_omni(bool omni);
    const Config& get_config();
}
