// Host-native unit tests for the DFU upload progress helpers
// (pedal_core/dfu_progress.hpp). The bootloader main.cpp is hardware-bound and
// not host-compilable, so the percent/format logic lives in a header-only unit that
// both the bootloader and this test include directly.

#include <unity.h>
#include <cstdint>
#include <cstring>

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

// The transport prefix, and the width it has to live inside: the status line is 128 px of
// a 6 px font, so 21 characters. "MIDI Received 100%" is the longest form the screen can
// ever be asked to show, and it has to fit without the tail being clipped off.
void test_format_received_names_the_transport(void) {
    char buf[24];
    dfu::format_received(buf, 0,   "USB");  TEST_ASSERT_EQUAL_STRING("USB Received 0%", buf);
    dfu::format_received(buf, 42,  "USB");  TEST_ASSERT_EQUAL_STRING("USB Received 42%", buf);
    dfu::format_received(buf, 100, "MIDI"); TEST_ASSERT_EQUAL_STRING("MIDI Received 100%", buf);
    TEST_ASSERT_TRUE(strlen(buf) <= 21u);
}

// No transport is not the same as an empty one: the screen shown before any host has
// claimed the session has no wire to name, and must not carry a stray leading space.
void test_format_received_without_a_transport_is_unprefixed(void) {
    char buf[24];
    dfu::format_received(buf, 42, nullptr); TEST_ASSERT_EQUAL_STRING("Received 42%", buf);
    dfu::format_received(buf, 42);          TEST_ASSERT_EQUAL_STRING("Received 42%", buf);
}

// The status line the percent form is built on, used directly for the states that carry no
// percentage. The returned length is where format_received resumes writing digits, so it
// has to count the prefix and the separator too.
void test_format_status(void) {
    char buf[24];
    TEST_ASSERT_EQUAL_UINT8(16, dfu::format_status(buf, "MIDI", "Updating..."));
    TEST_ASSERT_EQUAL_STRING("MIDI Updating...", buf);

    TEST_ASSERT_EQUAL_UINT8(15, dfu::format_status(buf, "USB", "Updating..."));
    TEST_ASSERT_EQUAL_STRING("USB Updating...", buf);

    TEST_ASSERT_EQUAL_UINT8(11, dfu::format_status(buf, nullptr, "Updating..."));
    TEST_ASSERT_EQUAL_STRING("Updating...", buf);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_percent_unknown_total_is_zero);
    RUN_TEST(test_percent_endpoints);
    RUN_TEST(test_percent_saturates);
    RUN_TEST(test_percent_truncates_toward_zero);
    RUN_TEST(test_percent_large_image);
    RUN_TEST(test_format_received);
    RUN_TEST(test_format_received_names_the_transport);
    RUN_TEST(test_format_received_without_a_transport_is_unprefixed);
    RUN_TEST(test_format_status);
    return UNITY_END();
}
