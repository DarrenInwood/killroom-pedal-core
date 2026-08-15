// Host-native unit tests for the expression-pedal interpolation (app/expr_map.hpp).
//
// This integer mapping turns the expression pedal's position into a parameter value
// within a configured [min,max] window in the Pedal's apply_expr_value(). It lives in a
// header-only helper (app/expr_map.hpp) so these tests can pin the endpoints, the
// reversed-pedal (min > max) case, the rounding, and the output range.
//
// The assertions are written against PARAM_MAX rather than a literal, so they keep their
// meaning if the parameter scale changes — and so the arithmetic is exercised at the
// width it actually runs at (the intermediate product is PARAM_MAX squared, which is why
// interp() works in int32).

#include <unity.h>
#include <cstdint>
#include <pedal_core/expr_map.hpp>

void setUp(void) {}
void tearDown(void) {}

// A window covering the middle of the range, in scale-relative terms.
static constexpr uint16_t WIN_LO = PARAM_MAX / 6u;        // ~17% of full scale
static constexpr uint16_t WIN_HI = 5u * PARAM_MAX / 6u;   // ~83%

// Pedal endpoints hit the window endpoints exactly (forward mapping).
void test_endpoints_forward(void) {
    TEST_ASSERT_EQUAL_UINT16(WIN_LO, expr_map::interp(WIN_LO, WIN_HI, 0));
    TEST_ASSERT_EQUAL_UINT16(WIN_HI, expr_map::interp(WIN_LO, WIN_HI, PARAM_MAX));
}

// Full-range window passes the pedal value straight through.
void test_full_range_passthrough(void) {
    TEST_ASSERT_EQUAL_UINT16(0,            expr_map::interp(0, PARAM_MAX, 0));
    TEST_ASSERT_EQUAL_UINT16(PARAM_MAX,    expr_map::interp(0, PARAM_MAX, PARAM_MAX));
    TEST_ASSERT_EQUAL_UINT16(PARAM_CENTRE, expr_map::interp(0, PARAM_MAX, PARAM_CENTRE));
}

// Midpoint pedal lands mid-window and the map is monotonic non-decreasing.
void test_midpoint_and_monotonic_forward(void) {
    const uint16_t want = (uint16_t)(WIN_LO + (WIN_HI - WIN_LO) / 2u);
    const uint16_t mid  = expr_map::interp(WIN_LO, WIN_HI, PARAM_CENTRE);
    TEST_ASSERT_TRUE(mid + 2u >= want && mid <= want + 2u);
    uint16_t prev = 0;
    for (uint32_t p = 0; p <= PARAM_MAX; ++p) {
        const uint16_t v = expr_map::interp(WIN_LO, WIN_HI, (uint16_t)p);
        TEST_ASSERT_TRUE(v >= prev);
        TEST_ASSERT_TRUE(v >= WIN_LO && v <= WIN_HI);
        prev = v;
    }
}

// Reversed pedal (min > max): output descends from min to max as the pedal rises,
// hitting both endpoints and staying monotonic non-increasing.
void test_reversed_pedal(void) {
    TEST_ASSERT_EQUAL_UINT16(WIN_HI, expr_map::interp(WIN_HI, WIN_LO, 0));          // pedal min -> min_val
    TEST_ASSERT_EQUAL_UINT16(WIN_LO, expr_map::interp(WIN_HI, WIN_LO, PARAM_MAX));  // pedal max -> max_val
    uint16_t prev = PARAM_MAX;
    for (uint32_t p = 0; p <= PARAM_MAX; ++p) {
        const uint16_t v = expr_map::interp(WIN_HI, WIN_LO, (uint16_t)p);
        TEST_ASSERT_TRUE(v <= prev);
        TEST_ASSERT_TRUE(v >= WIN_LO && v <= WIN_HI);
        prev = v;
    }
}

// A forward and a reversed sweep are mirror images: rounding must not favour one
// direction, which truncation used to do (it biases toward zero, so a forward sweep
// read low and a reversed one read high).
void test_rounding_is_symmetric(void) {
    for (uint32_t p = 0; p <= PARAM_MAX; ++p) {
        const uint16_t fwd = expr_map::interp(WIN_LO, WIN_HI, (uint16_t)p);
        const uint16_t rev = expr_map::interp(WIN_HI, WIN_LO, (uint16_t)p);
        // The two land symmetrically about the window, within a count of rounding.
        const uint32_t sum = (uint32_t)fwd + rev;
        TEST_ASSERT_TRUE(sum + 1u >= (uint32_t)WIN_LO + WIN_HI &&
                         sum <= (uint32_t)WIN_LO + WIN_HI + 1u);
    }
}

// Output never leaves the parameter range across the whole input space.
void test_output_stays_in_param_range(void) {
    const uint16_t windows[][2] = {
        {0, PARAM_MAX}, {0, 0}, {PARAM_MAX, PARAM_MAX}, {1, (uint16_t)(PARAM_MAX - 1u)},
        {(uint16_t)(PARAM_MAX - 1u), 1},
    };
    for (auto& w : windows)
        for (uint32_t p = 0; p <= PARAM_MAX; ++p) {
            const uint16_t v = expr_map::interp(w[0], w[1], (uint16_t)p);
            TEST_ASSERT_TRUE(v <= PARAM_MAX);
        }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_endpoints_forward);
    RUN_TEST(test_full_range_passthrough);
    RUN_TEST(test_midpoint_and_monotonic_forward);
    RUN_TEST(test_reversed_pedal);
    RUN_TEST(test_rounding_is_symmetric);
    RUN_TEST(test_output_stays_in_param_range);
    return UNITY_END();
}
