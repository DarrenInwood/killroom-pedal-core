#include <pedal_core/midi_responder_base.hpp>
#include <pedal_core/midi_handler.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // MIDI_CC_NRPN_* / MIDI_CC_DATA_ENTRY_* / NRPN_BANK_PARAMS

using pedal_core::MidiResponderBase;

void MidiResponderBase::send_cc(uint8_t cc, uint8_t value)
{
    const uint8_t ch = midi_handler::get_config().channel;
    const uint8_t msg[3] = {(uint8_t)(0xB0u | ch), cc, value};
    usb_midi::send(msg, 3);
    uart::write(msg[0]); uart::write(msg[1]); uart::write(msg[2]);
}

void MidiResponderBase::send_sysex(const uint8_t* buf, uint16_t len)
{
    usb_midi::send_sysex(buf, len);
    for (uint16_t i = 0; i < len; ++i) uart::write(buf[i]);
}

void MidiResponderBase::send_param_nrpn(uint8_t idx, uint16_t value)
{
    const uint8_t st = (uint8_t)(0xB0u | midi_handler::get_config().channel);
    const uint8_t quad[4][2] = {
        { MIDI_CC_NRPN_MSB,       NRPN_BANK_PARAMS         },
        { MIDI_CC_NRPN_LSB,       (uint8_t)(idx & 0x7Fu)   },
        { MIDI_CC_DATA_ENTRY_MSB, (uint8_t)((value >> 7) & 0x7Fu) },
        { MIDI_CC_DATA_ENTRY_LSB, (uint8_t)(value & 0x7Fu) },
    };
    for (const auto& m : quad) {
        const uint8_t msg[3] = { st, m[0], m[1] };
        usb_midi::send(msg, 3);
        uart::write(msg[0]); uart::write(msg[1]); uart::write(msg[2]);
    }
}
