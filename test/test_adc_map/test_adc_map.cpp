// Host-native unit tests for the ADC-code -> parameter-value mapping (app/adc_map.hpp).
//
// This pure integer math maps every knob and the expression pedal in the Pedal's
// process_adc(). It lives in a header-only, CMSIS-free helper (app/adc_map.hpp) so
// these tests can pin the endpoints, clamping, the degenerate-window fallback, and
// monotonicity, catching a refactor of the hot-path mapping at build time.
//
// The assertions are written against PARAM_MAX rather than a literal, so they keep
// their meaning if the parameter scale changes.

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <pedal_core/adc_map.hpp>

void setUp(void) {}
void tearDown(void) {}

// Full-range pass-through hits both endpoints exactly.
void test_raw_to_param_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT16(0,         adc_map::raw_to_param(0));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, adc_map::raw_to_param(4095));
}

// Pass-through is monotonic non-decreasing across the whole 12-bit range.
void test_raw_to_param_monotonic(void) {
    uint16_t prev = 0;
    for (uint32_t raw = 0; raw <= 4095u; ++raw) {
        const uint16_t v = adc_map::raw_to_param((uint16_t)raw);
        TEST_ASSERT_TRUE(v >= prev);
        TEST_ASSERT_TRUE(v <= PARAM_MAX);
        prev = v;
    }
}

// The 12-bit converter is finer than the parameter scale, so every parameter value is
// reachable from some ADC code — the knob is not quantised by this mapping.
void test_raw_to_param_reaches_every_value(void) {
    bool seen[PARAM_MAX + 1] = {};
    for (uint32_t raw = 0; raw <= 4095u; ++raw)
        seen[adc_map::raw_to_param((uint16_t)raw)] = true;
    for (uint16_t v = 0; v <= PARAM_MAX; ++v) {
        char msg[32];
        snprintf(msg, sizeof(msg), "value %u unreachable", (unsigned)v);
        TEST_ASSERT_TRUE_MESSAGE(seen[v], msg);
    }
}

// A calibrated window maps its own endpoints to the full parameter range.
void test_calibrated_window_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT16(0,         adc_map::calibrated_to_param(500, 500, 3500));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, adc_map::calibrated_to_param(3500, 500, 3500));
}

// Values outside the window clamp to the window endpoints (not wrap / overflow).
void test_calibrated_clamps_outside_window(void) {
    TEST_ASSERT_EQUAL_UINT16(0,         adc_map::calibrated_to_param(0,    500, 3500));  // below lo
    TEST_ASSERT_EQUAL_UINT16(0,         adc_map::calibrated_to_param(499,  500, 3500));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX, adc_map::calibrated_to_param(4095, 500, 3500));  // above hi
}

// A mid-window code lands mid-range; the map is monotonic across the window.
void test_calibrated_midpoint_and_monotonic(void) {
    const uint16_t lo = 500, hi = 3500;
    const uint16_t mid = adc_map::calibrated_to_param((uint16_t)((lo + hi) / 2), lo, hi);
    TEST_ASSERT_TRUE(mid >= PARAM_MID - 2u && mid <= PARAM_CENTRE + 2u);
    uint16_t prev = 0;
    for (uint32_t raw = lo; raw <= hi; ++raw) {
        const uint16_t v = adc_map::calibrated_to_param((uint16_t)raw, lo, hi);
        TEST_ASSERT_TRUE(v >= prev);
        TEST_ASSERT_TRUE(v <= PARAM_MAX);
        prev = v;
    }
}

// A degenerate window (hi <= lo) falls back to the uncalibrated full-range map,
// never dividing by zero.
void test_calibrated_degenerate_window_falls_back(void) {
    // hi == lo
    TEST_ASSERT_EQUAL_UINT16(adc_map::raw_to_param(2048), adc_map::calibrated_to_param(2048, 1000, 1000));
    // hi < lo (inverted / garbage)
    TEST_ASSERT_EQUAL_UINT16(adc_map::raw_to_param(4095), adc_map::calibrated_to_param(4095, 3000, 1000));
    // default blank-EEPROM window 0..4095 is full-range pass-through
    TEST_ASSERT_EQUAL_UINT16(adc_map::raw_to_param(1234), adc_map::calibrated_to_param(1234, 0, 4095));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_raw_to_param_endpoints);
    RUN_TEST(test_raw_to_param_monotonic);
    RUN_TEST(test_raw_to_param_reaches_every_value);
    RUN_TEST(test_calibrated_window_endpoints);
    RUN_TEST(test_calibrated_clamps_outside_window);
    RUN_TEST(test_calibrated_midpoint_and_monotonic);
    RUN_TEST(test_calibrated_degenerate_window_falls_back);
    return UNITY_END();
}
