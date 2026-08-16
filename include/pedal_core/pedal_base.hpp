#pragma once
#include <cstdint>
#include "ialgorithm.hpp"
#include "param_scale.hpp"

// The family's MIDI meanings: what a Control Change, a Program Change and a
// SysEx frame DO on a pedal. The base owns the machinery every product speaks
// identically —
//
//   - the NRPN path: parameter select on CC 99/98 latched until changed, RPN
//     traffic and the null NRPN disarming it, data entry MSB applying alone at
//     7-bit resolution (value << 7) with a following LSB refining it;
//   - bank select (either CC, last one wins, sticky across PCs) and the
//     bank*128+program slot arithmetic, out-of-range ignored rather than
//     wrapped, and a PC to the current slot with no unsaved edits a no-op;
//   - the bypass CC and the algorithm-select CC bounded by the product's count;
//   - SysEx dispatch: F0 <mfr> <device> <cmd> ... F7 header validation, then a
//     {cmd, min_len, handler} table walk over the product's table (short
//     frames drop silently).
//
// What a parameter write DOES (zone snapping, settle latching, display,
// echo), what selecting an algorithm means, and every SysEx command body stay
// in the product, behind the hooks.
namespace pedal_core {

class PedalBase {
public:
    void on_midi_cc(uint8_t cc, uint8_t value);
    void on_midi_program_change(uint8_t program);
    void on_midi_sysex(const uint8_t* data, uint16_t len);

    // The value scales, named here because this is where a host meets them.
    // The definitions are param_scale.hpp's, shared with the outbound echo.
    static uint16_t cc_to_param(uint8_t v)      { return param_scale::from_cc(v); }
    static uint16_t nrpn_to_param(uint16_t v14) { return param_scale::from_nrpn(v14); }
    static uint16_t param_to_nrpn(uint16_t v)   { return param_scale::to_nrpn(v); }

protected:
    ~PedalBase() = default;

    struct SysexEntry {
        uint8_t cmd;
        uint8_t min_len;   // payload bytes (between <cmd> and F7) the handler needs
        void (PedalBase::*fn)(const uint8_t* payload, uint16_t plen);
    };

    // --- what the product supplies ------------------------------------------
    virtual IAlgorithm& algo() = 0;
    virtual uint8_t     algorithm_count() const = 0;
    virtual uint16_t    preset_count() const = 0;
    virtual uint16_t    current_slot() const = 0;
    virtual bool        preset_dirty() const = 0;

    // A full-width parameter write from MIDI (NRPN data or a param CC). The
    // product's write path owns clamping and side effects.
    virtual void apply_param_from_midi(uint8_t idx, uint16_t value) = 0;

    // The algorithm-select CC, already bounds-checked against
    // algorithm_count(). Selection policy — defaults, carried-over slots,
    // page resets — is the product's.
    virtual void select_algorithm_from_midi(uint8_t id) = 0;

    // A Program Change resolved to an in-range slot that is not the
    // current-and-clean one.
    virtual void load_slot_from_midi(uint16_t slot) = 0;

    // The bypass CC's meaning (most products: relay active >= 64).
    virtual void set_bypass_from_midi(bool active) = 0;

    // Every CC the base machinery does not consume (a product's expression
    // CCs, its extra assignments). Default: ignored.
    virtual void on_device_cc(uint8_t cc, uint8_t value) { (void)cc; (void)value; }

    // The product's SysEx command table and identity bytes.
    virtual const SysexEntry* sysex_table(uint8_t& count) const = 0;
    virtual uint8_t sysex_device_id() const = 0;

    // The product's CC map, served from its midi_protocol constants.
    struct CcMap {
        uint8_t bypass;        // relay CC
        uint8_t algo_select;   // algorithm/voice select CC
        uint8_t param_base;    // first of the contiguous parameter CCs
        uint8_t param_count;   // how many parameter CCs follow param_base
    };
    virtual CcMap cc_map() const = 0;

private:
    bool nrpn_latched() const { return m_nrpn_msb != 0xFFu && m_nrpn_lsb != 0xFFu; }
    void nrpn_clear()         { m_nrpn_msb = 0xFFu; m_nrpn_lsb = 0xFFu; }
    void nrpn_apply_value(uint16_t value);

    uint8_t m_nrpn_msb      = 0xFFu;
    uint8_t m_nrpn_lsb      = 0xFFu;
    uint8_t m_nrpn_data_msb = 0u;
    uint8_t m_midi_bank     = 0u;
};

}  // namespace pedal_core
