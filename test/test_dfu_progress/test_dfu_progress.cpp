// Host-native unit tests for the DFU upload progress helpers
// (bootloader/src/dfu_progress.hpp). The bootloader main.cpp is hardware-bound and
// not host-compilable, so the percent/format logic lives in a header-only unit that
// both the bootloader and this test include directly.

#include <unity.h>
#include <cstdint>

#include <pedal_core/dfu_progress.hpp>

void setUp(void) {}
void tearDown(void) {}

// total == 0 means no FW_BEGIN was received (size unknown) → always 0%.
void test_percent_unknown_total_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, dfu::percent(0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, dfu::percent(12345, 0));
}

void test_percent_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT8(0,   dfu::percent(0, 1000));
    TEST_ASSERT_EQUAL_UINT8(50,  dfu::percent(500, 1000));
    TEST_ASSERT_EQUAL_UINT8(100, dfu::percent(1000, 1000));
}

// done > total (a late/duplicate chunk past the declared size) saturates, never wraps.
void test_percent_saturates(void) {
    TEST_ASSERT_EQUAL_UINT8(100, dfu::percent(1500, 1000));
}

// Matches the firmware integer division: floor toward zero.
void test_percent_truncates_toward_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(9, dfu::percent(99, 1000));   // 9.9% -> 9%
}

// A near-complete 512 KB image: done is bounded by the app flash region, so
// done*100 (< ~5.1e7) fits in uint32 with no overflow — the helper stays 32-bit
// (avoiding a 64-bit divide in the bootloader) and the result is still exact.
void test_percent_large_image(void) {
    TEST_ASSERT_EQUAL_UINT8(99,  dfu::percent(520000, 524288));
    TEST_ASSERT_EQUAL_UINT8(100, dfu::percent(524288, 524288));
}

void test_format_received(void) {
    char buf[16];
    dfu::format_received(buf, 0);   TEST_ASSERT_EQUAL_STRING("Received 0%", buf);
    dfu::format_received(buf, 7);   TEST_ASSERT_EQUAL_STRING("Received 7%", buf);
    dfu::format_received(buf, 42);  TEST_ASSERT_EQUAL_STRING("Received 42%", buf);
    dfu::format_received(buf, 100); TEST_ASSERT_EQUAL_STRING("Received 100%", buf);
    dfu::format_received(buf, 200); TEST_ASSERT_EQUAL_STRING("Received 100%", buf);  // clamped
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_percent_unknown_total_is_zero);
    RUN_TEST(test_percent_endpoints);
    RUN_TEST(test_percent_saturates);
    RUN_TEST(test_percent_truncates_toward_zero);
    RUN_TEST(test_percent_large_image);
    RUN_TEST(test_format_received);
    return UNITY_END();
}
