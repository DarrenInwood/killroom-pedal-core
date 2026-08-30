// The absent side of the feature switches, compiled.
//
// The assertions here are almost beside the point: what this suite is for is that the
// binary builds and links at all. Its env compiles tap_tempo.cpp, tempo_controller.cpp,
// tempo_led.cpp, midi_clock_out.cpp and external_input.cpp with both optional domains
// switched off, so each is an empty translation unit, and everything the library offers a
// product that wants neither domain still has to stand up beside them.
//
// Under the __has_include guards this branch was never built by anything. test/support puts
// every config header on the include path, so `pio test -e native` only ever compiled the
// present side of all four guards -- the trap CLAUDE.md warned about was structurally
// invisible to the suite that was supposed to protect against it.
//
// Run by `pio test -e native_minimal`; the `native` env ignores this directory, because
// there both switches are on.

#include <unity.h>
#include <cstdint>

#include "pedal_core_features.hpp"

// midi_clock_out.cpp is not in the src filter -- it needs midi_handler symbols the native
// build does not carry, so test_midi_clock_out compiles it into its own TU behind transport
// stubs. With the tempo layer off it is an empty translation unit and needs no stubs at all,
// which makes this the one place its absent side gets built.
#include "../../src/midi_clock_out.cpp"

// Core modules, which no optional domain gates. Including them here is the assertion: a
// product that wants neither a tempo nor an external jack still gets all of this.
#include <pedal_core/crc16.hpp>
#include <pedal_core/action.hpp>
#include <pedal_core/param_scale.hpp>
#include <pedal_core/wire_protocol.hpp>

void setUp(void) {}
void tearDown(void) {}

void test_both_optional_domains_are_off(void) {
    TEST_ASSERT_EQUAL_INT(0, PEDAL_CORE_HAS_TEMPO);
    TEST_ASSERT_EQUAL_INT(0, PEDAL_CORE_HAS_EXTINPUT);
}

// The action vocabulary is the family's, not the jack's: a product with no external input
// still assigns its panel switches from it.
void test_the_action_vocabulary_survives_without_the_jack(void) {
    TEST_ASSERT_TRUE((uint8_t)pedal_core::action::Action::Count > 0u);
}

// And the wire contract, which a product speaks whatever domains it carries.
void test_the_wire_contract_survives_without_the_optional_domains(void) {
    using namespace pedal_core::wire;
    TEST_ASSERT_EQUAL_UINT8(0x7Du, MANUFACTURER_ID);
    TEST_ASSERT_EQUAL_UINT16(GLOBAL_FRAME_MAX, global_frame_max());
}

void test_crc16_survives_without_the_optional_domains(void) {
    const uint8_t check[9] = { '1','2','3','4','5','6','7','8','9' };
    TEST_ASSERT_EQUAL_UINT16(0x29B1u, crc16_ccitt(check, 9u));   // the header's own check value
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_both_optional_domains_are_off);
    RUN_TEST(test_the_action_vocabulary_survives_without_the_jack);
    RUN_TEST(test_the_wire_contract_survives_without_the_optional_domains);
    RUN_TEST(test_crc16_survives_without_the_optional_domains);
    return UNITY_END();
}
