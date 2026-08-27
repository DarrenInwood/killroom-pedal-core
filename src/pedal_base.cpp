#include <pedal_core/pedal_base.hpp>
#include "pedal_core_ui_config.hpp"   // MIDI_CC_* / NRPN_BANK_PARAMS / PARAM_MAX (via the product's headers)

using pedal_core::PedalBase;

void PedalBase::nrpn_apply_value(uint16_t value)
{
    if (!nrpn_latched() || m_nrpn_msb != NRPN_BANK_PARAMS) return;
    if (m_nrpn_lsb >= algo().num_params()) return;
    if (value > PARAM_MAX) value = PARAM_MAX;
    apply_param_from_midi(m_nrpn_lsb, value);
}

void PedalBase::on_midi_cc(uint8_t cc, uint8_t value)
{
    // NRPN, the high-resolution parameter path. The parameter number latches until it
    // is changed; the data entry then writes it. An RPN select or the null NRPN clears
    // the latch, which is what stops a host's RPN traffic — the pitch-bend-range and
    // RPN Null sequences every DAW emits — from landing on a parameter.
    if (cc == MIDI_CC_RPN_MSB || cc == MIDI_CC_RPN_LSB) {
        nrpn_clear();
        return;                                   // no RPN these pedals implement
    }
    if (cc == MIDI_CC_NRPN_MSB || cc == MIDI_CC_NRPN_LSB) {
        if (cc == MIDI_CC_NRPN_MSB) m_nrpn_msb = value; else m_nrpn_lsb = value;
        if (m_nrpn_msb == 0x7Fu && m_nrpn_lsb == 0x7Fu) nrpn_clear();   // null NRPN
        return;
    }
    if (cc == MIDI_CC_DATA_INC || cc == MIDI_CC_DATA_DEC) return;       // not implemented
    if (cc == MIDI_CC_DATA_ENTRY_MSB) {
        // The MSB alone is a complete value at 7-bit resolution, scaled onto
        // the parameter range exactly like a plain CC — a controller that
        // sends only the MSB still sweeps the whole parameter, 127 landing on
        // PARAM_MAX. The LSB that may follow refines it to the full 14-bit
        // scale. Ignored outright when no parameter is selected.
        m_nrpn_data_msb = value;
        nrpn_apply_value(cc_to_param(value));
        return;
    }
    if (cc == MIDI_CC_DATA_ENTRY_LSB) {
        // The pair is a full-scale 14-bit value, 0..16383 across the whole
        // parameter range, scaled onto PARAM_MAX.
        nrpn_apply_value(nrpn_to_param((uint16_t)(((uint16_t)m_nrpn_data_msb << 7) | value)));
        return;
    }

    if (cc == MIDI_CC_BANK_MSB || cc == MIDI_CC_BANK_LSB) {
        // Either bank CC sets the pending bank, last one wins. It applies at
        // the next Program Change.
        m_midi_bank = value;
        return;
    }

    const CcMap map = cc_map();

    if (cc == map.bypass) {
        set_bypass_from_midi(value >= 64u);
        return;
    }

    if (cc == map.algo_select && value < algorithm_count()) {
        select_algorithm_from_midi(value);
        return;
    }

    if (cc >= map.param_base && cc < (uint8_t)(map.param_base + map.param_count)) {
        const uint8_t idx = (uint8_t)(cc - map.param_base);
        if (idx < algo().num_params())
            apply_param_from_midi(idx, cc_to_param(value));
        return;
    }

    on_device_cc(cc, value);
}

void PedalBase::on_midi_program_change(uint8_t program)
{
    // The offset shifts the whole map, banks included, so a 1-based controller
    // addressing bank 1 program 0 still reaches the slot below its bank boundary
    // rather than falling off the bottom of it.
    const int32_t addressed = (int32_t)(m_midi_bank * 128u + program) - (int32_t)pc_offset();
    if (addressed < 0) return;                             // below the first slot
    const uint16_t slot = (uint16_t)addressed;
    if (slot >= preset_count()) return;                    // ignored, not wrapped
    if (slot == current_slot() && !preset_dirty()) return; // no-op: no audio duck
    load_slot_from_midi(slot);
}

bool PedalBase::slot_to_pc(uint16_t slot, uint8_t& bank, uint8_t& program) const
{
    const uint32_t addressed = (uint32_t)slot + pc_offset();
    if (addressed >= 128u * 128u) return false;
    bank    = (uint8_t)(addressed / 128u);
    program = (uint8_t)(addressed % 128u);
    return true;
}

void PedalBase::on_midi_sysex(const uint8_t* data, uint16_t len)
{
    // F0 <mfr> <device> <cmd> <payload...> F7 — anything else is not ours.
    if (len < 5u) return;
    if (data[0] != 0xF0u || data[1] != SYSEX_MANUFACTURER_ID ||
        data[2] != sysex_device_id() || data[len - 1u] != 0xF7u) return;
    const uint8_t  cmd = data[3];
    const uint8_t* payload = &data[4];
    const uint16_t plen = (uint16_t)(len - 5u);
    uint8_t n = 0;
    const SysexEntry* table = sysex_table(n);
    for (uint8_t i = 0; i < n; ++i) {
        if (table[i].cmd != cmd) continue;
        if (plen < table[i].min_len) return;               // short frames drop silently
        // Past the manufacturer, the device ID, the terminator, a command this firmware
        // knows and a payload long enough to be it: whatever happens inside the handler,
        // this frame was addressed here and is being acted on.
        midi_accepted();
        (this->*table[i].fn)(payload, plen);
        return;
    }
}
