#include <pedal_core/midi_responder_base.hpp>
#include <pedal_core/midi_handler.hpp>
#include <pedal_core/param_scale.hpp>   // the shared data-entry scale
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // MIDI_CC_NRPN_* / MIDI_CC_DATA_ENTRY_* / MIDI_CC_BANK_* / NRPN_BANK_PARAMS

using pedal_core::MidiResponderBase;

namespace {

// One channel message onto both transports. USB takes it as a packet; DIN goes
// through the router, which decides whether the jack carries the pedal's own
// traffic and keeps the message whole against an inbound frame in flight.
void send_msg(uint8_t status, uint8_t d0, uint8_t d1)
{
    const uint8_t msg[3] = { status, d0, d1 };
    usb_midi::send(msg, 3);
    midi_handler::send_own(msg, 3);
}

uint8_t cc_status()
{
    return (uint8_t)(0xB0u | midi_handler::tx_channel());
}

}  // namespace

void MidiResponderBase::send_cc(uint8_t cc, uint8_t value)
{
    send_msg(cc_status(), cc, value);
}

void MidiResponderBase::send_echo_cc(uint8_t cc, uint8_t value)
{
    if (!midi_handler::get_config().tx_params) return;
    send_cc(cc, value);
}

void MidiResponderBase::send_sysex(const uint8_t* buf, uint16_t len)
{
    usb_midi::send_sysex(buf, len);
    midi_handler::send_own(buf, len);
}

void MidiResponderBase::send_program_change(uint8_t bank, uint8_t program)
{
    send_cc(MIDI_CC_BANK_MSB, 0u);
    send_cc(MIDI_CC_BANK_LSB, (uint8_t)(bank & 0x7Fu));
    const uint8_t msg[2] = { (uint8_t)(0xC0u | midi_handler::tx_channel()),
                             (uint8_t)(program & 0x7Fu) };
    usb_midi::send(msg, 2);
    midi_handler::send_own(msg, 2);
}

void MidiResponderBase::send_param_nrpn(uint8_t idx, uint16_t value)
{
    if (!midi_handler::get_config().tx_params) return;

    // The data-entry pair carries the full 14-bit scale: the parameter value
    // maps onto 0..16383 with exact endpoints, and the inbound path's
    // nrpn_to_param() maps it back to the identical value, so an echo a host
    // reflects at the pedal lands where it started.
    const uint16_t v14 = param_scale::to_nrpn(value);
    const uint8_t quad[4][2] = {
        { MIDI_CC_NRPN_MSB,       NRPN_BANK_PARAMS         },
        { MIDI_CC_NRPN_LSB,       (uint8_t)(idx & 0x7Fu)   },
        { MIDI_CC_DATA_ENTRY_MSB, (uint8_t)((v14 >> 7) & 0x7Fu) },
        { MIDI_CC_DATA_ENTRY_LSB, (uint8_t)(v14 & 0x7Fu) },
    };
    const uint8_t st = cc_status();
    for (const auto& m : quad) send_msg(st, m[0], m[1]);
}
