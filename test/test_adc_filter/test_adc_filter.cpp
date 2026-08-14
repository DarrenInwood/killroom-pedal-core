// Host-native unit tests for the ADC exponential-moving-average filter
// (drivers/adc_filter.hpp).
//
// This fixed-point 1/64-alpha smoother runs on every knob/pedal sample in
// adc.cpp's update_all(). It lives in a header-only, CMSIS-free helper
// (drivers/adc_filter.hpp) so these tests can pin the midpoint seed, steady-state
// convergence, lag, and overflow safety.

#include <unity.h>
#include <cstdint>
#include <pedal_core/adc_filter.hpp>

void setUp(void) {}
void tearDown(void) {}

// seed() scales x64; a steady raw input equal to the seeded value is a fixed point.
void test_seed_is_steady_state(void) {
    uint32_t acc = adc_filter::seed(2048u);   // driver midpoint init
    TEST_ASSERT_EQUAL_UINT32(2048u << 6, acc);
    for (int n = 0; n < 50; ++n)
        TEST_ASSERT_EQUAL_UINT16(2048u, adc_filter::step(acc, 2048u));
}

// From the midpoint seed a higher constant input converges up to (and stays at)
// the raw value — and the very first step lags below the target (alpha = 1/64).
void test_converges_and_lags_upward(void) {
    uint32_t acc = adc_filter::seed(2048u);
    const uint16_t first = adc_filter::step(acc, 4095u);
    TEST_ASSERT_TRUE(first > 2048u && first < 4095u);   // moved toward target, not all the way
    uint16_t out = first;
    for (int n = 0; n < 800; ++n) out = adc_filter::step(acc, 4095u);
    TEST_ASSERT_UINT16_WITHIN(1, 4095u, out);           // settled at the top (~64-sample tail)
}

// Symmetric: from a high seed a low constant input converges down to it.
void test_converges_downward(void) {
    uint32_t acc = adc_filter::seed(4000u);
    uint16_t out = 4000u;
    for (int n = 0; n < 800; ++n) out = adc_filter::step(acc, 0u);
    TEST_ASSERT_UINT16_WITHIN(1, 0u, out);
}

// Monotone non-overshooting approach: each step moves toward the target and never
// past it when starting below.
void test_monotone_no_overshoot(void) {
    uint32_t acc = adc_filter::seed(0u);
    uint16_t prev = 0u;
    for (int n = 0; n < 100; ++n) {
        const uint16_t out = adc_filter::step(acc, 4095u);
        TEST_ASSERT_TRUE(out >= prev);     // never decreases while climbing
        TEST_ASSERT_TRUE(out <= 4095u);    // never overshoots the target
        prev = out;
    }
}

// At the 12-bit ceiling the accumulator stays well inside uint32_t (max ~64*4095).
void test_no_overflow_at_full_scale(void) {
    uint32_t acc = adc_filter::seed(4095u);
    for (int n = 0; n < 2000; ++n) adc_filter::step(acc, 4095u);
    TEST_ASSERT_TRUE(acc <= (4095u << 6) + 64u);  // bounded; no wraparound
    TEST_ASSERT_EQUAL_UINT16(4095u, adc_filter::step(acc, 4095u));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_seed_is_steady_state);
    RUN_TEST(test_converges_and_lags_upward);
    RUN_TEST(test_converges_downward);
    RUN_TEST(test_monotone_no_overshoot);
    RUN_TEST(test_no_overflow_at_full_scale);
    return UNITY_END();
}
