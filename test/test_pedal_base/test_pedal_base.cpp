// Host-native unit tests for the family MIDI meanings (src/pedal_base.cpp),
// centred on the value scales: a 7-bit CC and a full-scale 14-bit NRPN
// data-entry pair both sweep the whole parameter range, with exact endpoints
// and a bit-exact round trip against the outbound scale. Driven through a
// minimal concrete pedal that records what the machinery asks of it.

#include <unity.h>
#include <cstdint>

#include "pedal_core_ui_config.hpp"
#include "../../src/pedal_base.cpp"

using pedal_core::PedalBase;
using pedal_core::IAlgorithm;

namespace {

class StubAlgorithm final : public IAlgorithm {
public:
    const char* name() const override { return "Stub"; }
    uint8_t     num_params() const override { return 16; }
    uint16_t    get_param(uint8_t) const override { return 0; }
    const char* param_name(uint8_t) const override { return "P"; }
    void        param_display(uint8_t, uint16_t, char* buf, uint8_t len) const override
    {
        if (len) buf[0] = '\0';
    }
    uint16_t default_param(uint8_t) const override { return 0; }
    void     reset() override {}
};

class TestPedal final : public PedalBase {
public:
    StubAlgorithm m_algo;
    uint8_t  last_idx   = 0xFFu;
    uint16_t last_value = 0xFFFFu;
    int      writes     = 0;

    IAlgorithm& algo() override { return m_algo; }
    uint8_t  algorithm_count() const override { return 1; }
    uint16_t preset_count() const override    { return 16; }
    uint16_t current_slot() const override    { return 0; }
    bool     preset_dirty() const override    { return false; }
    void     apply_param_from_midi(uint8_t idx, uint16_t value) override
    {
        last_idx = idx; last_value = value; ++writes;
    }
    void select_algorithm_from_midi(uint8_t) override {}
    void load_slot_from_midi(uint16_t) override {}
    void set_bypass_from_midi(bool) override {}
    const SysexEntry* sysex_table(uint8_t& count) const override { count = 0; return nullptr; }
    uint8_t sysex_device_id() const override { return 0x01; }
    CcMap cc_map() const override { return { 14u, 15u, 102u, 16u }; }
};

TestPedal* g_p = nullptr;

void select_param(uint8_t idx)
{
    g_p->on_midi_cc(MIDI_CC_NRPN_MSB, NRPN_BANK_PARAMS);
    g_p->on_midi_cc(MIDI_CC_NRPN_LSB, idx);
}

}  // namespace

void setUp(void)    { g_p = new TestPedal(); }
void tearDown(void) { delete g_p; g_p = nullptr; }

// ---------------------------------------------------------------------------

// The three scales share the contract: exact endpoints, rounding to nearest.
void test_scale_endpoints_are_exact(void) {
    TEST_ASSERT_EQUAL_UINT16(0u,        PedalBase::cc_to_param(0u));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, PedalBase::cc_to_param(127u));
    TEST_ASSERT_EQUAL_UINT16(0u,        PedalBase::nrpn_to_param(0u));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, PedalBase::nrpn_to_param(16383u));
    TEST_ASSERT_EQUAL_UINT16(0u,        PedalBase::param_to_nrpn(0u));
    TEST_ASSERT_EQUAL_UINT16(16383u,    PedalBase::param_to_nrpn(PARAM_MAX));
}

// Every parameter value survives the wire: out on the 14-bit scale and back
// through the inbound scale, bit-exact. This is what makes the pedal's own
// echo safe to reflect at it.
void test_nrpn_round_trip_is_identity(void) {
    for (uint32_t v = 0; v <= PARAM_MAX; ++v)
        TEST_ASSERT_EQUAL_UINT16((uint16_t)v,
                                 PedalBase::nrpn_to_param(PedalBase::param_to_nrpn((uint16_t)v)));
}

// The inbound scale is monotonic: a host sweeping 0..16383 never sees the
// parameter step backwards.
void test_nrpn_scale_is_monotonic(void) {
    uint16_t prev = 0;
    for (uint32_t v = 0; v <= 16383u; ++v) {
        const uint16_t p = PedalBase::nrpn_to_param((uint16_t)v);
        TEST_ASSERT_TRUE(p >= prev);
        prev = p;
    }
    // A full sweep reaches every parameter value (14 bits covers 10 with room).
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, prev);
}

// A full-scale 14-bit write lands scaled: 16383 -> PARAM_MAX, half scale near
// centre — not clamped at the parameter width.
void test_data_entry_pair_is_full_scale(void) {
    select_param(3u);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 127u);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_LSB, 127u);   // 16383
    TEST_ASSERT_EQUAL_UINT8(3u, g_p->last_idx);
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, g_p->last_value);

    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 64u);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_LSB, 0u);     // 8192, just past half
    TEST_ASSERT_EQUAL_UINT16(PedalBase::nrpn_to_param(8192u), g_p->last_value);
}

// The MSB alone behaves like a plain CC — 127 sweeps to PARAM_MAX — and the
// LSB that follows refines it on the full 14-bit scale.
void test_msb_alone_scales_like_a_cc_then_lsb_refines(void) {
    select_param(0u);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 127u);
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, g_p->last_value);

    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 5u);
    TEST_ASSERT_EQUAL_UINT16(PedalBase::cc_to_param(5u), g_p->last_value);

    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_LSB, 9u);
    TEST_ASSERT_EQUAL_UINT16(PedalBase::nrpn_to_param((5u << 7) | 9u), g_p->last_value);
}

// RPN traffic and the null NRPN disarm the latch, so the data entry that
// follows writes nothing.
void test_rpn_and_null_nrpn_disarm(void) {
    select_param(0u);
    g_p->on_midi_cc(MIDI_CC_RPN_MSB, 0u);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 100u);
    TEST_ASSERT_EQUAL_INT(0, g_p->writes);

    select_param(0u);
    g_p->on_midi_cc(MIDI_CC_NRPN_MSB, 0x7Fu);
    g_p->on_midi_cc(MIDI_CC_NRPN_LSB, 0x7Fu);
    g_p->on_midi_cc(MIDI_CC_DATA_ENTRY_MSB, 100u);
    TEST_ASSERT_EQUAL_INT(0, g_p->writes);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_scale_endpoints_are_exact);
    RUN_TEST(test_nrpn_round_trip_is_identity);
    RUN_TEST(test_nrpn_scale_is_monotonic);
    RUN_TEST(test_data_entry_pair_is_full_scale);
    RUN_TEST(test_msb_alone_scales_like_a_cc_then_lsb_refines);
    RUN_TEST(test_rpn_and_null_nrpn_disarm);
    return UNITY_END();
}
