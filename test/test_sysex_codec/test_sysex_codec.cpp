// Host-native unit tests for the DFU bootloader's SysEx firmware-update codec
// (bootloader/src/sysex_codec.hpp).
//
// The bootloader is a separate CMSIS/TinyUSB build that the native env never
// compiles, so its two correctness-critical pure functions — the Roland-style
// 7-bit unpack and the CRC32 image verify — were factored into a host-portable
// header to be locked down here. A wrong decode mangles the flashed image; a
// wrong CRC accepts a corrupt one or rejects a good one — either bricks the
// update path, which has no in-field recovery short of a BOOT0 re-flash.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <pedal_core/sysex_codec.hpp>

void setUp(void) {}
void tearDown(void) {}

// --- decode_7bit -----------------------------------------------------------

void test_decode_full_group_no_high_bits(void) {
    // One full group: MSB byte 0x00 then 7 data bytes, all <0x80 -> verbatim.
    const uint8_t in[8] = {0x00, 1, 2, 3, 4, 5, 6, 7};
    uint8_t out[7] = {};
    const uint16_t n = sysex_codec::decode_7bit(in, out, sizeof(in), sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(7, n);
    for (uint8_t i = 0; i < 7; ++i) TEST_ASSERT_EQUAL_UINT8(i + 1, out[i]);
}

void test_decode_msb_bits_set_all(void) {
    // MSB byte 0x7F sets bit7 of all 7 data bytes; 0x00 data -> 0x80 each.
    const uint8_t in[8] = {0x7F, 0, 0, 0, 0, 0, 0, 0};
    uint8_t out[7] = {};
    const uint16_t n = sysex_codec::decode_7bit(in, out, sizeof(in), sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(7, n);
    for (uint8_t i = 0; i < 7; ++i) TEST_ASSERT_EQUAL_HEX8(0x80, out[i]);
}

void test_decode_partial_final_group_distributes_msbs(void) {
    // The 5-byte CRC layout: 1 MSB byte + 4 data bytes -> 4 decoded.
    // MSB 0x05 = bits 0 and 2 set, so data[0] and data[2] gain bit7.
    const uint8_t in[5] = {0x05, 0x2A, 0x3B, 0x4C, 0x5D};
    uint8_t out[4] = {};
    const uint16_t n = sysex_codec::decode_7bit(in, out, sizeof(in), sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(4, n);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);  // 0x2A | 0x80
    TEST_ASSERT_EQUAL_HEX8(0x3B, out[1]);  // 0x3B | 0x00
    TEST_ASSERT_EQUAL_HEX8(0xCC, out[2]);  // 0x4C | 0x80
    TEST_ASSERT_EQUAL_HEX8(0x5D, out[3]);  // 0x5D | 0x00
}

void test_decode_two_groups_full_plus_partial(void) {
    // 8-byte full group (7 data) + 5-byte partial group (4 data) = 11 decoded.
    const uint8_t in[13] = {0x00, 1, 2, 3, 4, 5, 6, 7,
                            0x00, 8, 9, 10, 11};
    uint8_t out[16] = {};
    const uint16_t n = sysex_codec::decode_7bit(in, out, sizeof(in), sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(11, n);
    for (uint8_t i = 0; i < 11; ++i) TEST_ASSERT_EQUAL_UINT8(i + 1, out[i]);
}

void test_decode_respects_out_cap(void) {
    // A full group offered but a 3-byte sink: must stop at out_cap, no overrun.
    const uint8_t in[8] = {0x00, 1, 2, 3, 4, 5, 6, 7};
    uint8_t out[4] = {0xEE, 0xEE, 0xEE, 0xEE};  // sentinel in the 4th slot
    const uint16_t n = sysex_codec::decode_7bit(in, out, sizeof(in), 3);
    TEST_ASSERT_EQUAL_UINT16(3, n);
    TEST_ASSERT_EQUAL_UINT8(1, out[0]);
    TEST_ASSERT_EQUAL_UINT8(2, out[1]);
    TEST_ASSERT_EQUAL_UINT8(3, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[3]);  // untouched
}

void test_decode_empty_input(void) {
    uint8_t out[4] = {};
    TEST_ASSERT_EQUAL_UINT16(0, sysex_codec::decode_7bit(nullptr, out, 0, sizeof(out)));
}

void test_decode_lone_msb_byte_yields_nothing(void) {
    // A group header with no following data bytes decodes to zero output.
    const uint8_t in[1] = {0x7F};
    uint8_t out[4] = {};
    TEST_ASSERT_EQUAL_UINT16(0, sysex_codec::decode_7bit(in, out, 1, sizeof(out)));
}

// --- decoded_size ----------------------------------------------------------

void test_decoded_size_matches_decode_for_all_lengths(void) {
    // decoded_size must return exactly what an untruncated decode_7bit produces —
    // it is the invariant the bootloader's oversized-chunk guard relies on to
    // NACK before decoding, so any drift would let a truncating chunk slip through.
    uint8_t in[128];
    for (uint16_t i = 0; i < sizeof(in); ++i) in[i] = (uint8_t)i;  // MSB bytes clear -> verbatim
    uint8_t out[128] = {};
    for (uint16_t len = 0; len <= sizeof(in); ++len) {
        const uint16_t actual = sysex_codec::decode_7bit(in, out, len, sizeof(out));
        TEST_ASSERT_EQUAL_UINT16(actual, sysex_codec::decoded_size(len));
    }
}

void test_decoded_size_exceeds_buffer_flags_truncation(void) {
    // A chunk sized so its decode overshoots a fixed buffer is what the guard must
    // catch: decoded_size reports the true (untruncated) length > buffer, while a
    // decode into that buffer clamps and hides the overflow.
    const uint16_t cap = 1024;
    // encoded_len whose decode is one byte over the cap: 1024*8/7 rounds up.
    const uint16_t encoded = 1176;  // decodes to 1176 - ceil(1176/8) = 1176 - 147 = 1029
    TEST_ASSERT_GREATER_THAN_UINT16(cap, sysex_codec::decoded_size(encoded));
    // And a chunk that fits exactly is not flagged.
    TEST_ASSERT_EQUAL_UINT16(cap, sysex_codec::decoded_size(1171));  // 1171 - 147 = 1024
}

// --- crc32_update ----------------------------------------------------------

void test_crc32_standard_check_value(void) {
    // The canonical CRC-32 check value: "123456789" -> 0xCBF43926.
    const char* s = "123456789";
    const uint32_t crc = sysex_codec::crc32_update(0, (const uint8_t*)s, 9);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

void test_crc32_empty_is_zero(void) {
    // Init ^ final XOR cancel over zero bytes, matching a zero-length firmware.
    const uint8_t dummy = 0;
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, sysex_codec::crc32_update(0, &dummy, 0));
}

void test_crc32_detects_single_bit_flip(void) {
    uint8_t buf[16];
    for (uint8_t i = 0; i < 16; ++i) buf[i] = i;
    const uint32_t a = sysex_codec::crc32_update(0, buf, sizeof(buf));
    buf[7] ^= 0x01;  // flip one bit
    const uint32_t b = sysex_codec::crc32_update(0, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(a, b);
}


// --- encode_7bit -----------------------------------------------------------

void test_encode_leads_each_group_with_its_high_bits(void) {
    const uint8_t in[7] = {0x80, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[8] = {};
    const uint16_t n = sysex_codec::encode_7bit(in, out, sizeof(in), sizeof(out));
    TEST_ASSERT_EQUAL_UINT16(8, n);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[0]);   // bits 0 and 2 -> bytes 0 and 2 were high
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, out[3]);
}

void test_encode_keeps_every_byte_sysex_safe(void) {
    uint8_t in[64] = {};
    for (uint16_t i = 0; i < sizeof(in); ++i) in[i] = (uint8_t)(i * 7u);
    uint8_t out[128] = {};
    const uint16_t n = sysex_codec::encode_7bit(in, out, sizeof(in), sizeof(out));
    for (uint16_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_HEX8(0, out[i] & 0x80u);
}

void test_encode_round_trips_through_decode_at_every_length(void) {
    for (uint16_t len = 0; len <= 32u; ++len) {
        uint8_t in[32] = {};
        for (uint16_t i = 0; i < len; ++i) in[i] = (uint8_t)(i * 37u + 11u);
        uint8_t enc[64] = {};
        uint8_t dec[32] = {};
        const uint16_t n = sysex_codec::encode_7bit(in, enc, len, sizeof(enc));
        TEST_ASSERT_EQUAL_UINT16(sysex_codec::encoded_size(len), n);
        const uint16_t m = sysex_codec::decode_7bit(enc, dec, n, sizeof(dec));
        TEST_ASSERT_EQUAL_UINT16(len, m);
        for (uint16_t i = 0; i < len; ++i) TEST_ASSERT_EQUAL_HEX8(in[i], dec[i]);
    }
}

void test_encode_packs_a_96_bit_uid_into_14_bytes(void) {
    uint8_t uid[12] = {};
    for (uint8_t i = 0; i < sizeof(uid); ++i) uid[i] = (uint8_t)(0x11u * (i + 1u));
    uint8_t out[16] = {};
    TEST_ASSERT_EQUAL_UINT16(14, sysex_codec::encode_7bit(uid, out, sizeof(uid), sizeof(out)));
    TEST_ASSERT_EQUAL_UINT16(14, sysex_codec::encoded_size(12));
}

void test_encode_refuses_rather_than_truncating(void) {
    // A partial encode would decode to something else entirely, so a short
    // buffer must produce nothing at all.
    const uint8_t in[7] = {1, 2, 3, 4, 5, 6, 7};
    uint8_t out[4] = {};
    TEST_ASSERT_EQUAL_UINT16(0, sysex_codec::encode_7bit(in, out, sizeof(in), sizeof(out)));
}

void test_encode_empty_input_is_empty(void) {
    uint8_t out[4] = {};
    TEST_ASSERT_EQUAL_UINT16(0, sysex_codec::encode_7bit(nullptr, out, 0u, sizeof(out)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_full_group_no_high_bits);
    RUN_TEST(test_decode_msb_bits_set_all);
    RUN_TEST(test_decode_partial_final_group_distributes_msbs);
    RUN_TEST(test_decode_two_groups_full_plus_partial);
    RUN_TEST(test_decode_respects_out_cap);
    RUN_TEST(test_decode_empty_input);
    RUN_TEST(test_decode_lone_msb_byte_yields_nothing);
    RUN_TEST(test_decoded_size_matches_decode_for_all_lengths);
    RUN_TEST(test_decoded_size_exceeds_buffer_flags_truncation);
    RUN_TEST(test_crc32_standard_check_value);
    RUN_TEST(test_crc32_empty_is_zero);
    RUN_TEST(test_crc32_detects_single_bit_flip);
    RUN_TEST(test_encode_leads_each_group_with_its_high_bits);
    RUN_TEST(test_encode_keeps_every_byte_sysex_safe);
    RUN_TEST(test_encode_round_trips_through_decode_at_every_length);
    RUN_TEST(test_encode_packs_a_96_bit_uid_into_14_bytes);
    RUN_TEST(test_encode_refuses_rather_than_truncating);
    RUN_TEST(test_encode_empty_input_is_empty);
    return UNITY_END();
}
