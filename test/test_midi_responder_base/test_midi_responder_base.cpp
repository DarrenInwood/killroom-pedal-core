// Host-native unit tests for the transport half of a pedal's MIDI responder
// (src/midi_responder_base.cpp).
//
// This module is small, and everything it owns is wire-visible: the byte order of the NRPN
// quad, Bank MSB then Bank LSB then Program Change, and which of its five entry points the
// tx_params switch is allowed to silence. A host editor reads all of it, so a change here
// that nothing pins is a change a product only finds against somebody's DAW.
//
// It is a thin adapter onto two link-time namespaces, so the suite is the pattern
// test_midi_clock_out uses: the .cpp compiled into this TU behind recording stubs for the
// calls it makes.

#include <unity.h>
#include <cstdint>
#include <vector>

#include <pedal_core/midi_handler.hpp>

// --- recording stubs for what the responder reaches through -----------------

namespace midi_handler {
    static Config               g_config;
    static std::vector<uint8_t> g_jack;        // the router's side, as a byte run
    static uint8_t              g_tx_channel = 0u;

    void send_own(const uint8_t* msg, uint16_t len) { g_jack.insert(g_jack.end(), msg, msg + len); }
    const Config& get_config() { return g_config; }
    uint8_t tx_channel() { return g_tx_channel; }
}

namespace usb_midi {
    static std::vector<uint8_t>              g_usb;         // channel messages, run together
    static std::vector<std::vector<uint8_t>> g_usb_sysex;   // whole frames, one entry apiece

    void send(const uint8_t* msg, uint8_t len) { g_usb.insert(g_usb.end(), msg, msg + len); }
    void send_sysex(const uint8_t* frame, uint16_t len) { g_usb_sysex.emplace_back(frame, frame + len); }
}

#include "../../src/midi_responder_base.cpp"

// A product's responder is a derived class; the base has a protected destructor so that is
// the only way to hold one. Nothing here overrides anything -- the transport half is all
// the base's.
class Responder : public pedal_core::MidiResponderBase {};

static Responder* g_r = nullptr;

void setUp(void) {
    midi_handler::g_config     = midi_handler::Config{};
    midi_handler::g_tx_channel = 0u;
    midi_handler::g_jack.clear();
    usb_midi::g_usb.clear();
    usb_midi::g_usb_sysex.clear();
    g_r = new Responder();
}
void tearDown(void) { delete g_r; g_r = nullptr; }

// Both transports carry the same bytes, which is the whole contract of this module: a host
// on USB and a pedal on the jack are told the same thing.
static bool both_carry(std::vector<uint8_t> expected) {
    return midi_handler::g_jack == expected && usb_midi::g_usb == expected;
}

// ---------------------------------------------------------------------------
// The transport primitive
// ---------------------------------------------------------------------------

// One Control Change, on the transmit channel, onto both transports.
void test_a_cc_goes_out_on_the_transmit_channel(void) {
    midi_handler::g_tx_channel = 5u;
    g_r->send_cc(7u, 64u);
    TEST_ASSERT_TRUE(both_carry({ 0xB5, 7u, 64u }));
}

// ---------------------------------------------------------------------------
// What tx_params is allowed to silence
//
// The switch exists to keep a knob's own chatter off a shared chain. It is NOT
// carries-policy -- it separates one kind of the pedal's traffic from another, which is
// why it lives here rather than in the router's table.
// ---------------------------------------------------------------------------

// The echo of a control the player just moved is exactly what the switch is for.
void test_tx_params_off_silences_the_control_echo(void) {
    midi_handler::g_config.tx_params = false;
    g_r->send_echo_cc(7u, 64u);
    TEST_ASSERT_TRUE(midi_handler::g_jack.empty());
    TEST_ASSERT_TRUE(usb_midi::g_usb.empty());

    midi_handler::g_config.tx_params = true;
    g_r->send_echo_cc(7u, 64u);
    TEST_ASSERT_TRUE(both_carry({ 0xB0, 7u, 64u }));
}

// The NRPN quad is an echo of a knob as well, and rides the same switch.
void test_tx_params_off_silences_the_nrpn_echo(void) {
    midi_handler::g_config.tx_params = false;
    g_r->send_param_nrpn(3u, 512u);
    TEST_ASSERT_TRUE(midi_handler::g_jack.empty());
}

// A message the product MEANT to send is not chatter, and the switch does not reach it.
void test_tx_params_off_does_not_silence_a_deliberate_cc(void) {
    midi_handler::g_config.tx_params = false;
    g_r->send_cc(7u, 64u);
    TEST_ASSERT_TRUE_MESSAGE(both_carry({ 0xB0, 7u, 64u }),
                             "tx_params silenced a message the product meant to send");
}

// A reply a host asked for is not chatter either. This is the one the header states
// outright, and the reason tx_params cannot be folded into the router's carries-policy:
// Src::Self cannot tell a solicited reply from an unasked-for echo.
void test_tx_params_off_does_not_silence_a_sysex_reply(void) {
    midi_handler::g_config.tx_params = false;
    const uint8_t frame[5] = { 0xF0u, 0x7Du, 0x01u, 0x70u, 0xF7u };
    g_r->send_sysex(frame, 5u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)midi_handler::g_jack.size(),
                                  "tx_params silenced a reply a host asked for");
    TEST_ASSERT_EQUAL_INT(1, (int)usb_midi::g_usb_sysex.size());
    TEST_ASSERT_EQUAL_INT(5, (int)usb_midi::g_usb_sysex[0].size());
}

// Nor a preset change, which is state a host is being told about rather than a knob.
void test_tx_params_off_does_not_silence_a_program_change(void) {
    midi_handler::g_config.tx_params = false;
    g_r->send_program_change(2u, 9u);
    TEST_ASSERT_FALSE(midi_handler::g_jack.empty());
}

// ---------------------------------------------------------------------------
// The byte orders a host editor depends on
// ---------------------------------------------------------------------------

// Bank Select then Program Change, and the pair is spec-correct on the way out: CC 0 (MSB)
// zero, CC 32 (LSB) the bank, then the PC. A host that reads them in any other order lands
// on the wrong preset.
void test_a_program_change_is_bank_msb_then_lsb_then_the_change(void) {
    midi_handler::g_tx_channel = 1u;
    g_r->send_program_change(2u, 9u);
    TEST_ASSERT_TRUE(both_carry({
        0xB1, MIDI_CC_BANK_MSB, 0u,
        0xB1, MIDI_CC_BANK_LSB, 2u,
        0xC1, 9u,
    }));
}

// The NRPN quad: select the parameter (CC 99 then 98), then its value (CC 6 then 38). The
// order is the spec's, and a host reading the data entry before the select writes it to
// whatever parameter was selected last.
void test_the_nrpn_quad_selects_the_parameter_before_sending_the_value(void) {
    const uint16_t value = 512u;
    const uint16_t v14   = pedal_core::param_scale::to_nrpn(value);

    g_r->send_param_nrpn(3u, value);
    TEST_ASSERT_TRUE(both_carry({
        0xB0, MIDI_CC_NRPN_MSB,       NRPN_BANK_PARAMS,
        0xB0, MIDI_CC_NRPN_LSB,       3u,
        0xB0, MIDI_CC_DATA_ENTRY_MSB, (uint8_t)((v14 >> 7) & 0x7Fu),
        0xB0, MIDI_CC_DATA_ENTRY_LSB, (uint8_t)(v14 & 0x7Fu),
    }));
}

// The quad carries the full 14-bit scale, so an echo a host reflects back at the pedal
// lands on the value it started from. The endpoints are what a rounding error shows up in.
void test_the_nrpn_value_survives_the_round_trip(void) {
    for (uint16_t v : { (uint16_t)0u, (uint16_t)1u, (uint16_t)511u,
                        (uint16_t)512u, (uint16_t)(PARAM_MAX - 1u), PARAM_MAX }) {
        midi_handler::g_jack.clear();
        g_r->send_param_nrpn(0u, v);

        // Rebuild the 14-bit value from the two data-entry bytes actually written.
        const uint16_t msb = midi_handler::g_jack[8];
        const uint16_t lsb = midi_handler::g_jack[11];
        const uint16_t v14 = (uint16_t)((msb << 7) | lsb);
        TEST_ASSERT_EQUAL_UINT16(v, pedal_core::param_scale::from_nrpn(v14));
    }
}

// Every data byte the responder writes is a legal one: a status byte where a host expects
// data desynchronises its parser for the rest of the stream.
void test_no_data_byte_has_its_high_bit_set(void) {
    g_r->send_param_nrpn(0x7Fu, PARAM_MAX);
    for (size_t i = 0; i < midi_handler::g_jack.size(); ++i)
        if (i % 3u != 0u)
            TEST_ASSERT_TRUE_MESSAGE(midi_handler::g_jack[i] < 0x80u, "a data byte was a status byte");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_cc_goes_out_on_the_transmit_channel);
    RUN_TEST(test_tx_params_off_silences_the_control_echo);
    RUN_TEST(test_tx_params_off_silences_the_nrpn_echo);
    RUN_TEST(test_tx_params_off_does_not_silence_a_deliberate_cc);
    RUN_TEST(test_tx_params_off_does_not_silence_a_sysex_reply);
    RUN_TEST(test_tx_params_off_does_not_silence_a_program_change);
    RUN_TEST(test_a_program_change_is_bank_msb_then_lsb_then_the_change);
    RUN_TEST(test_the_nrpn_quad_selects_the_parameter_before_sending_the_value);
    RUN_TEST(test_the_nrpn_value_survives_the_round_trip);
    RUN_TEST(test_no_data_byte_has_its_high_bit_set);
    return UNITY_END();
}
