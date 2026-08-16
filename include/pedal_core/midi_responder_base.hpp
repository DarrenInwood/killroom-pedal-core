#pragma once
#include <cstdint>

// The transport half of a pedal's MIDI responder: everything here mirrors one
// message onto both transports (USB and DIN) on the handler's current channel.
// What a product SAYS — its preset dump, device info, global settings and
// version frames — is the product's, in its derived responder.
namespace pedal_core {

class MidiResponderBase {
public:
    // One Control Change on both transports.
    void send_cc(uint8_t cc, uint8_t value);

    // One complete SysEx frame (F0 ... F7) on both transports.
    void send_sysex(const uint8_t* buf, uint16_t len);

    // The NRPN quad: select the parameter (CC 99/98), then its value
    // (CC 6/38) on the full 14-bit data-entry scale — 0..16383 spans the
    // whole parameter range with exact endpoints, and the inbound path maps
    // it back to the identical parameter value. USB takes one message per
    // packet; the UART stream can share a running status byte but is written
    // plainly so both transports carry the same four messages.
    void send_param_nrpn(uint8_t idx, uint16_t value);

protected:
    ~MidiResponderBase() = default;
};

}  // namespace pedal_core
