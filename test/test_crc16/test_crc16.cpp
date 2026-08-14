// Host-native unit tests for the shared CRC-16/CCITT-FALSE (util/crc16.hpp).
//
// This is the integrity check every EEPROM block stores (preset records, the
// settings/calibration blocks, the last-slot ring, the header page). The
// standard check value locks the polynomial/init/reflection choices so the
// other storage suites may compute expected CRCs via this implementation.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <pedal_core/crc16.hpp>

void setUp(void) {}
void tearDown(void) {}

// The CRC-16/CCITT-FALSE catalogue check value: "123456789" -> 0x29B1.
void test_standard_check_value(void) {
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16_ccitt((const uint8_t*)s, 9));
}

// Empty span returns the init value untouched.
void test_empty_span(void) {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16_ccitt(nullptr, 0));
}

// Any single-bit flip in a block-sized span changes the CRC.
void test_single_bit_flip_detected(void) {
    uint8_t block[62];
    for (uint16_t i = 0; i < sizeof(block); ++i)
        block[i] = (uint8_t)(i * 7u + 3u);
    const uint16_t good = crc16_ccitt(block, sizeof(block));
    for (uint16_t byte = 0; byte < sizeof(block); ++byte)
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            block[byte] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_TRUE(crc16_ccitt(block, sizeof(block)) != good);
            block[byte] ^= (uint8_t)(1u << bit);
        }
    TEST_ASSERT_EQUAL_HEX16(good, crc16_ccitt(block, sizeof(block)));
}

// The degenerate erased/zeroed patterns produce distinct, non-degenerate CRCs
// (neither 0x0000 nor 0xFFFF), so an all-0xFF or all-0x00 block can't carry a
// CRC field that accidentally matches its own fill byte pattern.
void test_degenerate_patterns(void) {
    uint8_t ff[62], zero[62];
    memset(ff, 0xFF, sizeof(ff));
    memset(zero, 0x00, sizeof(zero));
    const uint16_t crc_ff   = crc16_ccitt(ff, sizeof(ff));
    const uint16_t crc_zero = crc16_ccitt(zero, sizeof(zero));
    TEST_ASSERT_TRUE(crc_ff != crc_zero);
    TEST_ASSERT_TRUE(crc_ff != 0xFFFFu && crc_ff != 0x0000u);
    TEST_ASSERT_TRUE(crc_zero != 0xFFFFu && crc_zero != 0x0000u);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_standard_check_value);
    RUN_TEST(test_empty_span);
    RUN_TEST(test_single_bit_flip_detected);
    RUN_TEST(test_degenerate_patterns);
    return UNITY_END();
}
