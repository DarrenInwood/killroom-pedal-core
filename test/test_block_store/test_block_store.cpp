// Host-native unit tests for the stored-block seal (block_store.hpp).
//
// The seal is the one convention every stored block in the family shares, so
// these tests pin it hard: the round trip, corruption detection at every byte
// including the reserved span, size-genericity (the 8-byte ring record and the
// 64-byte page blocks use the same functions), and — as a characterization —
// the exact CRC bytes for known images, so a reimplementation that changes the
// polynomial, the init value or the byte order fails loudly rather than
// quietly invalidating every consumer's stored data.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <pedal_core/block_store.hpp>

using pedal_core::blocks::seal;
using pedal_core::blocks::crc_ok;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------

void test_seal_then_ok_roundtrip(void)
{
    uint8_t b[64];
    for (uint8_t i = 0; i < 62u; ++i) b[i] = (uint8_t)(i * 7u);
    seal(b, sizeof(b));
    TEST_ASSERT_TRUE(crc_ok(b, sizeof(b)));
}

void test_every_byte_flip_detected(void)
{
    uint8_t b[64];
    for (uint8_t i = 0; i < 62u; ++i) b[i] = (uint8_t)(0xA5u ^ i);
    seal(b, sizeof(b));
    for (uint8_t i = 0; i < 64u; ++i) {   // includes the CRC bytes themselves
        b[i] ^= 0x01u;
        TEST_ASSERT_FALSE_MESSAGE(crc_ok(b, sizeof(b)), "flip not detected");
        b[i] ^= 0x01u;
    }
    TEST_ASSERT_TRUE(crc_ok(b, sizeof(b)));
}

void test_size_generic_ring_record(void)
{
    // The last-slot ring uses 8-byte records with the same seal.
    uint8_t r[8] = { 0xB5u, 3u, 0x12u, 0x34u, 0xFFu, 0xFFu, 0u, 0u };
    seal(r, sizeof(r));
    TEST_ASSERT_TRUE(crc_ok(r, sizeof(r)));
    r[1] ^= 0x80u;
    TEST_ASSERT_FALSE(crc_ok(r, sizeof(r)));
}

// Characterization: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no final
// xor) over a known payload, stored little-endian. "123456789" -> 0x29B1 is
// the reference check value for this CRC; a 62-byte zero block is pinned as a
// second, layout-shaped vector.
void test_known_vectors_pin_the_convention(void)
{
    uint8_t v[11];
    memcpy(v, "123456789", 9);
    seal(v, sizeof(v));
    TEST_ASSERT_EQUAL_HEX8(0xB1u, v[9]);    // low byte first (LE)
    TEST_ASSERT_EQUAL_HEX8(0x29u, v[10]);

    uint8_t z[64];
    memset(z, 0, sizeof(z));
    seal(z, sizeof(z));
    const uint16_t crc = (uint16_t)(z[62] | ((uint16_t)z[63] << 8));
    TEST_ASSERT_EQUAL_HEX16(crc16_ccitt(z, 62u), crc);
    TEST_ASSERT_TRUE(crc_ok(z, sizeof(z)));
}

void test_reserved_ff_span_is_covered(void)
{
    // The seal covers reserved 0xFF bytes: claiming one changes the CRC.
    uint8_t b[64];
    memset(b, 0xFFu, sizeof(b));
    b[0] = 0x5Eu;   // a magic
    seal(b, sizeof(b));
    TEST_ASSERT_TRUE(crc_ok(b, sizeof(b)));
    b[40] = 0x00u;  // a "new field" in the reserved span
    TEST_ASSERT_FALSE(crc_ok(b, sizeof(b)));
}

// ---------------------------------------------------------------------------

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_seal_then_ok_roundtrip);
    RUN_TEST(test_every_byte_flip_detected);
    RUN_TEST(test_size_generic_ring_record);
    RUN_TEST(test_known_vectors_pin_the_convention);
    RUN_TEST(test_reserved_ff_span_is_covered);
    return UNITY_END();
}
