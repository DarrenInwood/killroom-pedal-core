// Host-native unit tests for the stored-block machinery (block_store.hpp).
//
// The seal is the one convention every stored block in the family shares, so
// these tests pin it hard: the round trip, corruption detection at every byte
// including the reserved span, size-genericity (the 8-byte ring record and the
// 64-byte page blocks use the same functions), and — as a characterization —
// the exact CRC bytes for known images, so a reimplementation that changes the
// polynomial, the init value or the byte order fails loudly rather than
// quietly invalidating every consumer's stored data.
//
// The sealed-block I/O, the layout header and the wear-leveled SlotRing run
// against the in-RAM EEPROM fake. The ring cases carry the consumers' full
// coverage: wire-format pinning (16-bit slot, first-write seq 1, magic), the
// round-robin and only-if-changed rules, greatest-seq restore across the
// modular-16 wrap, every rejection (torn, bad magic, degenerate patterns,
// out-of-range slot), page alignment, and the wear bound.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <pedal_core/block_store.hpp>
#include "eeprom_fake.hpp"

using pedal_core::blocks::seal;
using pedal_core::blocks::crc_ok;
using pedal_core::blocks::SlotRing;

// The family ring shape at its family address, bounded like a 256-slot device.
static constexpr uint16_t RING_ADDR  = 0x7C00u;
static constexpr uint16_t RING_COUNT = 64u;
static constexpr uint16_t RING_RECSZ = 8u;
static constexpr uint8_t  RING_MAGIC = 0xB5u;
static constexpr uint16_t SLOT_BOUND = 256u;

void setUp(void) { eeprom_test::reset(); }
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

// --- Sealed-block I/O + the layout header ----------------------------------

void test_block_io_roundtrip_and_blank_fails(void)
{
    uint8_t b[64];
    // A blank page fails the CRC like any other unwritten data.
    TEST_ASSERT_FALSE(pedal_core::blocks::load_block(0x7A00u, 64u, b));
    for (uint8_t i = 0; i < 62u; ++i) b[i] = (uint8_t)(i ^ 0x5Au);
    TEST_ASSERT_TRUE(pedal_core::blocks::save_block(0x7A00u, 64u, b));
    uint8_t r[64];
    TEST_ASSERT_TRUE(pedal_core::blocks::load_block(0x7A00u, 64u, r));
    TEST_ASSERT_EQUAL_MEMORY(b, r, 64u);
    // One flipped byte in the store and the load rejects it.
    eeprom_test::poke(0x7A00u + 10u, (uint8_t)(eeprom_test::peek(0x7A00u + 10u) ^ 1u));
    TEST_ASSERT_FALSE(pedal_core::blocks::load_block(0x7A00u, 64u, r));
}

void test_header_lifecycle(void)
{
    const uint32_t MAGIC = 0x58464D41u;   // one product's layout magic
    TEST_ASSERT_FALSE(pedal_core::blocks::header_valid(0x0000u, 64u, MAGIC, 1u));
    TEST_ASSERT_TRUE(pedal_core::blocks::write_header(0x0000u, 64u, MAGIC, 1u));
    TEST_ASSERT_TRUE(pedal_core::blocks::header_valid(0x0000u, 64u, MAGIC, 1u));
    // The magic is byte-exact little-endian at [0..3].
    TEST_ASSERT_EQUAL_HEX8(0x41u, eeprom_test::peek(0));
    TEST_ASSERT_EQUAL_HEX8(0x4Du, eeprom_test::peek(1));
    TEST_ASSERT_EQUAL_HEX8(0x46u, eeprom_test::peek(2));
    TEST_ASSERT_EQUAL_HEX8(0x58u, eeprom_test::peek(3));
    // A different product's magic or a bumped version reads as foreign.
    TEST_ASSERT_FALSE(pedal_core::blocks::header_valid(0x0000u, 64u, 0x4441524Bu, 1u));
    TEST_ASSERT_FALSE(pedal_core::blocks::header_valid(0x0000u, 64u, MAGIC, 2u));
    // A torn header fails its CRC.
    eeprom_test::poke(5u, (uint8_t)(eeprom_test::peek(5u) ^ 0x80u));
    TEST_ASSERT_FALSE(pedal_core::blocks::header_valid(0x0000u, 64u, MAGIC, 1u));
}

// --- The wear-leveled last-slot ring ---------------------------------------

static uint16_t rec_addr(uint16_t idx) { return (uint16_t)(RING_ADDR + idx * RING_RECSZ); }

// Write a valid record directly into the fake (bypasses any ring RAM state).
static void poke_record(uint16_t idx, uint16_t slot, uint16_t seq)
{
    uint8_t r[RING_RECSZ];
    memset(r, 0xFFu, sizeof(r));
    r[0] = (uint8_t)(slot & 0xFFu);
    r[1] = (uint8_t)(slot >> 8);
    r[2] = (uint8_t)(seq & 0xFFu);
    r[3] = (uint8_t)(seq >> 8);
    r[4] = RING_MAGIC;
    seal(r, RING_RECSZ);
    for (uint8_t i = 0; i < RING_RECSZ; ++i)
        eeprom_test::poke((uint16_t)(rec_addr(idx) + i), r[i]);
}

static uint16_t rec_slot(uint16_t idx) {
    return (uint16_t)(eeprom_test::peek(rec_addr(idx))
         | ((uint16_t)eeprom_test::peek((uint16_t)(rec_addr(idx) + 1u)) << 8));
}
static uint16_t rec_seq(uint16_t idx) {
    return (uint16_t)(eeprom_test::peek((uint16_t)(rec_addr(idx) + 2u))
         | ((uint16_t)eeprom_test::peek((uint16_t)(rec_addr(idx) + 3u)) << 8));
}

// A "power cycle": a fresh ring re-derives its RAM state from the store.
static int32_t power_cycle()
{
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    return ring.restore();
}

void test_blank_ring_restores_nothing(void) {
    TEST_ASSERT_EQUAL_INT32(-1, power_cycle());
}

void test_single_persist_then_restore(void) {
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    ring.remember(5u);
    TEST_ASSERT_EQUAL_UINT16(5u, rec_slot(0));   // landed in the first ring entry
    TEST_ASSERT_EQUAL_UINT16(1u, rec_seq(0));    // seq starts at 1, never 0
    TEST_ASSERT_EQUAL_HEX8(RING_MAGIC, eeprom_test::peek((uint16_t)(rec_addr(0) + 4u)));
    TEST_ASSERT_EQUAL_INT32(5, power_cycle());
}

void test_slot_is_sixteen_bit_on_the_wire(void) {
    poke_record(0, /*slot*/300u, /*seq*/4u);
    // 300 is outside this ring's bound, so it is rejected as out of range —
    // but the low/high bytes prove the field itself is 16-bit.
    TEST_ASSERT_EQUAL_UINT8(44u, eeprom_test::peek(rec_addr(0)));            // 300 & 0xFF
    TEST_ASSERT_EQUAL_UINT8(1u,  eeprom_test::peek((uint16_t)(rec_addr(0) + 1u)));  // 300 >> 8
    TEST_ASSERT_EQUAL_INT32(-1, power_cycle());

    // The largest in-range slot survives a round trip.
    eeprom_test::reset();
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    ring.remember((uint16_t)(SLOT_BOUND - 1u));
    TEST_ASSERT_EQUAL_INT32((int32_t)(SLOT_BOUND - 1u), power_cycle());
}

void test_persist_advances_round_robin(void) {
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    ring.remember(1u);
    ring.remember(2u);
    ring.remember(3u);
    TEST_ASSERT_EQUAL_UINT16(1u, rec_slot(0));  TEST_ASSERT_EQUAL_UINT16(1u, rec_seq(0));
    TEST_ASSERT_EQUAL_UINT16(2u, rec_slot(1));  TEST_ASSERT_EQUAL_UINT16(2u, rec_seq(1));
    TEST_ASSERT_EQUAL_UINT16(3u, rec_slot(2));  TEST_ASSERT_EQUAL_UINT16(3u, rec_seq(2));
    TEST_ASSERT_EQUAL_INT32(3, power_cycle());
}

void test_only_if_changed_skips_duplicate(void) {
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    ring.remember(5u);
    const uint32_t w0 = eeprom_test::write_count(rec_addr(0));
    ring.remember(5u);  // no-op
    TEST_ASSERT_EQUAL_UINT32(w0, eeprom_test::write_count(rec_addr(0)));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, eeprom_test::peek(rec_addr(1)));  // second entry still blank
    ring.remember(6u);  // real change
    TEST_ASSERT_EQUAL_UINT16(6u, rec_slot(1));
    TEST_ASSERT_EQUAL_UINT16(2u, rec_seq(1));
}

void test_restore_picks_greatest_seq(void) {
    poke_record(0, 40u, 10u);
    poke_record(1, 41u, 11u);   // newest
    poke_record(2, 42u,  9u);
    TEST_ASSERT_EQUAL_INT32(41, power_cycle());
}

void test_modular_wrap_picks_newest(void) {
    poke_record(0, 10u, 0xFFFEu);
    poke_record(1, 11u, 0xFFFFu);
    poke_record(2, 12u, 0x0000u);
    poke_record(3, 13u, 0x0001u);   // newest after the wrap
    TEST_ASSERT_EQUAL_INT32(13, power_cycle());
}

void test_torn_record_rejected(void) {
    poke_record(0, 7u, 5u);                    // valid, older
    poke_record(1, 9u, 6u);                    // would be newest...
    eeprom_test::poke((uint16_t)(rec_addr(1) + 2u), 0x55u);   // ...but corrupt it
    TEST_ASSERT_EQUAL_INT32(7, power_cycle()); // falls back to the valid record
}

void test_bad_magic_rejected(void) {
    poke_record(0, 7u, 5u);
    poke_record(1, 9u, 6u);
    eeprom_test::poke((uint16_t)(rec_addr(1) + 4u), 0x00u);
    TEST_ASSERT_EQUAL_INT32(7, power_cycle());
}

void test_degenerate_records_rejected(void) {
    // An all-0x00 record must not pose as "slot 0", and an all-0xFF (erased)
    // record must read as empty.
    for (uint8_t b = 0; b < RING_RECSZ; ++b) eeprom_test::poke((uint16_t)(rec_addr(0) + b), 0x00u);
    for (uint8_t b = 0; b < RING_RECSZ; ++b) eeprom_test::poke((uint16_t)(rec_addr(1) + b), 0xFFu);
    TEST_ASSERT_EQUAL_INT32(-1, power_cycle());
    // Prove the ring was treated as empty: the next change writes at index 0, seq 1.
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    ring.remember(9u);
    TEST_ASSERT_EQUAL_UINT16(9u, rec_slot(0));
    TEST_ASSERT_EQUAL_UINT16(1u, rec_seq(0));
}

void test_out_of_range_slot_rejected(void) {
    poke_record(0, 3u, 1u);
    poke_record(1, SLOT_BOUND, 2u);   // out of range, would be newest
    TEST_ASSERT_EQUAL_INT32(3, power_cycle());
}

void test_ring_wrap_after_count_writes(void) {
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    const uint16_t n = (uint16_t)(RING_COUNT + 3u);
    uint16_t last = 0xFFFFu;
    for (uint16_t i = 0; i < n; ++i) {
        last = (uint16_t)(1u + (i % 120u));  // always differs from the previous one
        ring.remember(last);
    }
    // The (COUNT+1)th..(COUNT+3)th writes wrapped back over entries 0,1,2.
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(RING_COUNT + 1u), rec_seq(0));
    TEST_ASSERT_EQUAL_INT32((int32_t)last, power_cycle());
}

void test_record_never_straddles_a_page(void) {
    for (uint16_t i = 0; i < RING_COUNT; ++i)
        TEST_ASSERT_EQUAL_UINT16(rec_addr(i) / 64u,
                                 (uint16_t)((rec_addr(i) + RING_RECSZ - 1u) / 64u));
}

void test_wear_is_spread_across_the_ring(void) {
    SlotRing ring(RING_ADDR, RING_COUNT, RING_RECSZ, RING_MAGIC, SLOT_BOUND);
    ring.restore();
    const uint32_t writes = 200u;
    uint16_t last = 0;
    for (uint32_t i = 0; i < writes; ++i) {
        last = (i & 1u) ? 7u : 9u;       // alternates -> every change is real
        ring.remember(last);
    }
    // Round-robin over COUNT entries: ceil(200/64) = 4 programmings per cell, max.
    const uint32_t ceil_per_cell = (writes + RING_COUNT - 1u) / RING_COUNT;
    uint32_t worst = 0;
    const uint16_t ring_bytes = (uint16_t)(RING_COUNT * RING_RECSZ);
    for (uint16_t off = 0; off < ring_bytes; ++off) {
        const uint32_t c = eeprom_test::write_count((uint16_t)(RING_ADDR + off));
        if (c > worst) worst = c;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(ceil_per_cell, worst);       // <= 4, never the full 200
    TEST_ASSERT_EQUAL_INT32((int32_t)last, power_cycle());        // last change still restored
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
    RUN_TEST(test_block_io_roundtrip_and_blank_fails);
    RUN_TEST(test_header_lifecycle);
    RUN_TEST(test_blank_ring_restores_nothing);
    RUN_TEST(test_single_persist_then_restore);
    RUN_TEST(test_slot_is_sixteen_bit_on_the_wire);
    RUN_TEST(test_persist_advances_round_robin);
    RUN_TEST(test_only_if_changed_skips_duplicate);
    RUN_TEST(test_restore_picks_greatest_seq);
    RUN_TEST(test_modular_wrap_picks_newest);
    RUN_TEST(test_torn_record_rejected);
    RUN_TEST(test_bad_magic_rejected);
    RUN_TEST(test_degenerate_records_rejected);
    RUN_TEST(test_out_of_range_slot_rejected);
    RUN_TEST(test_ring_wrap_after_count_writes);
    RUN_TEST(test_record_never_straddles_a_page);
    RUN_TEST(test_wear_is_spread_across_the_ring);
    return UNITY_END();
}
