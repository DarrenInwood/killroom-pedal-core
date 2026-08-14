// Host-native unit tests for the quadrature decode logic (drivers/encoder_decode.hpp).
//
// This suite locks down the correctness-critical core in isolation — the
// quadrature transition table, factored into encoder_decode.hpp so it can be
// exercised without any CMSIS/EXTI machinery. A wrong table entry would make the
// encoder skip detents or count the wrong way. (The surrounding ISR path —
// detent division, button debounce/hold — is covered by test_encoder, which
// compiles the real encoder.cpp behind host register stubs.)

#include <unity.h>
#include <cstdint>
#include <initializer_list>
#include <pedal_core/encoder_decode.hpp>

void setUp(void) {}
void tearDown(void) {}

// State encoding: bit1 = A, bit0 = B.
static constexpr uint8_t S00 = 0;  // A=0 B=0
static constexpr uint8_t S01 = 1;  // A=0 B=1
static constexpr uint8_t S10 = 2;  // A=1 B=0
static constexpr uint8_t S11 = 3;  // A=1 B=1

// Sum the quarter-steps over a full sequence of states (consecutive transitions).
static int sum_sequence(std::initializer_list<uint8_t> states) {
    int total = 0;
    uint8_t prev = *states.begin();
    bool first = true;
    for (uint8_t s : states) {
        if (!first) total += encoder_decode::step(prev, s);
        prev = s;
        first = false;
    }
    return total;
}

void test_cw_detent_is_plus_four(void) {
    // CW full cycle 00 -> 10 -> 11 -> 01 -> 00 == one detent == +4 quarter-steps.
    const int sum = sum_sequence({S00, S10, S11, S01, S00});
    TEST_ASSERT_EQUAL_INT(4, sum);
}

void test_ccw_detent_is_minus_four(void) {
    // CCW full cycle 00 -> 01 -> 11 -> 10 -> 00 == one detent == -4 quarter-steps.
    const int sum = sum_sequence({S00, S01, S11, S10, S00});
    TEST_ASSERT_EQUAL_INT(-4, sum);
}

void test_no_change_is_zero(void) {
    for (uint8_t s = 0; s < 4; ++s)
        TEST_ASSERT_EQUAL_INT8(0, encoder_decode::step(s, s));
}

void test_double_step_transitions_are_zero(void) {
    // Diagonal transitions (both pins flip at once) are ambiguous/illegal and must
    // contribute 0 rather than a guessed direction.
    TEST_ASSERT_EQUAL_INT8(0, encoder_decode::step(S00, S11));
    TEST_ASSERT_EQUAL_INT8(0, encoder_decode::step(S11, S00));
    TEST_ASSERT_EQUAL_INT8(0, encoder_decode::step(S01, S10));
    TEST_ASSERT_EQUAL_INT8(0, encoder_decode::step(S10, S01));
}

void test_each_single_step_is_plus_or_minus_one(void) {
    // Every valid single-pin transition must read as exactly ±1, and a transition
    // and its reverse must be negatives of each other (antisymmetry).
    const uint8_t legal[4][2] = {{S00, S10}, {S10, S11}, {S11, S01}, {S01, S00}};
    for (auto& t : legal) {
        const int8_t fwd = encoder_decode::step(t[0], t[1]);
        const int8_t rev = encoder_decode::step(t[1], t[0]);
        TEST_ASSERT_EQUAL_INT8(1, fwd);
        TEST_ASSERT_EQUAL_INT8(-1, rev);
    }
}

void test_two_cw_detents_accumulate(void) {
    const int sum = sum_sequence(
        {S00, S10, S11, S01, S00, S10, S11, S01, S00});
    TEST_ASSERT_EQUAL_INT(8, sum);
}

void test_high_bits_masked(void) {
    // step() masks its inputs to 2 bits, so stray high bits don't index out of range.
    TEST_ASSERT_EQUAL_INT8(encoder_decode::step(S00, S10),
                           encoder_decode::step(0xFC | S00, 0xFC | S10));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cw_detent_is_plus_four);
    RUN_TEST(test_ccw_detent_is_minus_four);
    RUN_TEST(test_no_change_is_zero);
    RUN_TEST(test_double_step_transitions_are_zero);
    RUN_TEST(test_each_single_step_is_plus_or_minus_one);
    RUN_TEST(test_two_cw_detents_accumulate);
    RUN_TEST(test_high_bits_masked);
    return UNITY_END();
}
