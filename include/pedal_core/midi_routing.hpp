#pragma once
#include <cstdint>
#include "wire_protocol.hpp"
#include "midi_handler.hpp"

// The one place the MIDI routing block and the pedal's MIDI configuration meet.
//
// The block is twelve bytes on the wire; nine of them are what midi_handler runs on, and
// the other three belong elsewhere — the program-change offset is the base's, the clock
// switch is the generator's, and what the pedal announces is the product's policy. All
// twelve come back named, so nothing is quietly dropped and re-parsed from raw offsets in
// each product.
//
// This is also where the two spellings of "follow the receive channel" are reconciled:
// the wire carries 0x7F, because 0xFF is not a legal SysEx data byte, and the handler
// holds 0xFF. Neither is wrong and both are load-bearing, so the translation lives here
// rather than in whichever module happens to touch it.
//
// wire_protocol.hpp is the contract a host editor is built against and stays ignorant of
// the driver; midi_handler.hpp knows nothing of SysEx. This header is what knows both.
namespace pedal_core::routing {

// A routing block resolved into the things that act on it.
struct Settings {
    // The nine fields midi_handler runs on.
    midi_handler::Config config{};

    // The program that addresses slot 0 — PedalBase::pc_offset().
    uint8_t pc_offset = 0u;

    // Whether the pedal generates a MIDI clock — midi_clock_out::set_enabled().
    bool clock_out = false;

    // Which of its own state changes the pedal announces. Product policy: the values are
    // wire::midi_routing::tx_state::*, and what a pedal does with them is its own.
    uint8_t tx_state = wire::midi_routing::tx_state::OFF;
};

// The block as the pedal will run it. A value outside its range is clamped here rather
// than believed — the wire contract says a range is the pedal's to enforce, and a host
// that sends a fifth out-mode gets the default instead of undefined behaviour.
inline Settings from_wire(const wire::midi_routing::RoutingBlock& b)
{
    namespace wr = wire::midi_routing;
    Settings s;

    s.config.channel = (b.rx_channel < 16u) ? b.rx_channel : 0u;
    s.config.omni    = b.omni;

    // The follow sentinel changes spelling; an explicit channel is carried through, and
    // anything else is read as "follow", which is the safe reading of a byte we cannot
    // place: a pedal that speaks on the channel it listens to is never silent.
    s.config.tx_channel = (b.tx_channel < 16u)
                              ? b.tx_channel
                              : midi_handler::TX_CHANNEL_FOLLOW_RX;

    s.config.out_mode = (b.out < wr::out_mode::COUNT)
                            ? (midi_handler::OutMode)b.out
                            : midi_handler::OutMode::Merge;

    s.config.usb_din = (b.usb_din_route < wr::usb_din::COUNT)
                           ? (midi_handler::UsbDinRoute)b.usb_din_route
                           : midi_handler::UsbDinRoute::Off;

    s.config.clock_thru = b.clock_thru;
    s.config.rx_pc      = b.rx_pc;
    s.config.rx_sysex   = b.rx_sysex;
    s.config.tx_params  = b.tx_params;

    s.pc_offset = (uint8_t)(b.pc_offset & 0x7Fu);
    s.clock_out = b.clock_out;
    s.tx_state  = (b.tx < wr::tx_state::COUNT) ? b.tx : wr::tx_state::OFF;

    return s;
}

// The same settings as the block, so a host writes back exactly what it read.
inline wire::midi_routing::RoutingBlock to_wire(const Settings& s)
{
    wire::midi_routing::RoutingBlock b;

    b.rx_channel = (uint8_t)(s.config.channel & 0x0Fu);
    b.omni       = s.config.omni;
    b.tx_channel = (s.config.tx_channel == midi_handler::TX_CHANNEL_FOLLOW_RX)
                       ? wire::midi_routing::TX_CHANNEL_FOLLOW_RX
                       : (uint8_t)(s.config.tx_channel & 0x0Fu);

    b.out           = (uint8_t)s.config.out_mode;
    b.usb_din_route = (uint8_t)s.config.usb_din;
    b.clock_thru    = s.config.clock_thru;
    b.rx_pc         = s.config.rx_pc;
    b.rx_sysex      = s.config.rx_sysex;
    b.tx_params     = s.config.tx_params;

    b.pc_offset = (uint8_t)(s.pc_offset & 0x7Fu);
    b.clock_out = s.clock_out;
    b.tx        = s.tx_state;

    return b;
}

}  // namespace pedal_core::routing
