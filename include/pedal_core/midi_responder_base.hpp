#pragma once
#include <cstdint>

// The transport half of a pedal's MIDI responder: everything here mirrors one
// message onto both transports — USB directly, the jack through the handler's router,
// which owns whether the jack carries the pedal's own traffic at all. Messages
// go out on the transmit channel, which follows the receive channel unless the
// product has set one, so a pedal listening in Omni still speaks on one channel.
// What a product SAYS — its preset dump, device info, global settings and
// version frames — is the product's, in its derived responder.
namespace pedal_core {

class MidiResponderBase {
public:
    // One Control Change on both transports. The transport primitive: a product
    // uses it for the state messages it means to send.
    void send_cc(uint8_t cc, uint8_t value);

    // A Control Change reporting the pedal's own control position back to a host —
    // the traffic the tx_params switch exists to keep off a shared chain.
    void send_echo_cc(uint8_t cc, uint8_t value);

    // One complete SysEx frame (F0 ... F7) on both transports. Never gated by
    // tx_params: a reply a host asked for is not chatter.
    void send_sysex(const uint8_t* buf, uint16_t len);

    // Bank Select then Program Change, addressing one preset slot. The pair is
    // spec-correct on the way out — CC 0 (MSB) 0, CC 32 (LSB) the bank — while the
    // inbound path keeps accepting either byte, because controllers disagree and
    // only one of us has to be generous.
    void send_program_change(uint8_t bank, uint8_t program);

    // The NRPN quad: select the parameter (CC 99/98), then its value
    // (CC 6/38) on the full 14-bit data-entry scale — 0..16383 spans the
    // whole parameter range with exact endpoints, and the inbound path maps
    // it back to the identical parameter value. USB takes one message per
    // packet; the UART stream can share a running status byte but is written
    // plainly so both transports carry the same four messages. Gated by
    // tx_params, like every other echo of a knob the player just moved.
    void send_param_nrpn(uint8_t idx, uint16_t value);

protected:
    ~MidiResponderBase() = default;
};

}  // namespace pedal_core
