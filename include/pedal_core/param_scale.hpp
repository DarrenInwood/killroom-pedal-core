#pragma once
#include <cstdint>
#include "pedal_core_config.hpp"   // PARAM_MAX

// The parameter value scales — how a MIDI value becomes a parameter and back.
// Pure math, header-only, so the inbound path (pedal_base) and the outbound
// echo (midi_responder_base) share one definition without one depending on the
// other.
//
// Every scale rounds to nearest and hits both endpoints exactly, and the NRPN
// pair round-trips bit-exact: from_nrpn(to_nrpn(v)) == v for every parameter
// value. That is what lets a host reflect the pedal's own echo back at it —
// or an editor mirror a knob — without the value drifting a step per pass.
//
// A 7-bit CC and a 14-bit NRPN both sweep the WHOLE parameter range. Neither
// carries raw parameter counts, so a controller needs no knowledge of this
// product's parameter width, and a wider parameter scale costs nothing on the
// wire.
namespace pedal_core::param_scale {

// A 7-bit Control Change value: 127 lands exactly on PARAM_MAX.
inline uint16_t from_cc(uint8_t v)
{
    return (uint16_t)(((uint32_t)v * PARAM_MAX + 63u) / 127u);
}

// A full-scale 14-bit NRPN data-entry value: 16383 lands exactly on PARAM_MAX.
inline uint16_t from_nrpn(uint16_t v14)
{
    if (v14 > 16383u) v14 = 16383u;
    return (uint16_t)(((uint32_t)v14 * PARAM_MAX + 8191u) / 16383u);
}

// A parameter value onto the 14-bit data-entry scale. Inverse of from_nrpn.
inline uint16_t to_nrpn(uint16_t value)
{
    if (value > PARAM_MAX) value = PARAM_MAX;
    return (uint16_t)(((uint32_t)value * 16383u + PARAM_MAX / 2u) / PARAM_MAX);
}

}  // namespace pedal_core::param_scale
