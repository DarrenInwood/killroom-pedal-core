// Host-native unit tests for the DFU identify reply (pedal_core/dfu_protocol.hpp).
//
// A bootloader is typically not field-updatable, so whatever it can answer on the day it
// ships is what every host will ever be able to negotiate with. That makes two properties
// of this frame worth locking down harder than the field values themselves:
//
//   * every byte is <= 0x7F. A byte of 0x80 or above between F0 and F7 is a MIDI status
//     byte and aborts the frame, so a packing bug does not corrupt one field -- it takes
//     the whole reply off the wire, on hardware that cannot be patched.
//   * a reply longer than the reader expects parses fine. That is the only mechanism by
//     which a later bootloader can add a field without spending another command byte, and
//     it only works if hosts written today already behave that way.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <pedal_core/dfu_protocol.hpp>

void setUp() {}
void tearDown() {}

static const uint8_t UID[12] = {
    0x01, 0x7F, 0x80, 0xFF, 0x00, 0xA5, 0x5A, 0xC3, 0x3C, 0xF0, 0x0F, 0x99,
};

static dfu_protocol::Info sample()
{
    dfu_protocol::Info in{};
    in.manufacturer  = 0x7D;
    in.device        = 0x01;
    in.version_major = 1;
    in.version_minor = 2;
    in.version_patch = 3;
    in.uid           = UID;
    in.app_base      = 0x0800C000u;
    in.app_size      = 464u * 1024u;
    in.chunk_max     = 256u;
    in.flags         = 0;
    return in;
}

static void test_builds_the_declared_length()
{
    uint8_t out[64];
    TEST_ASSERT_EQUAL_UINT16(dfu_protocol::INFO_LEN,
                             dfu_protocol::build_info(out, sizeof(out), sample()));
}

// The bug class that silently kills a SysEx frame.
static void test_every_byte_is_seven_bit_safe()
{
    uint8_t out[64];
    const uint16_t n = dfu_protocol::build_info(out, sizeof(out), sample());
    TEST_ASSERT_TRUE(n > 0);
    for (uint16_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(out[i] <= 0x7F, "a payload byte would abort the frame");
    }
}

static void test_scalar_fields_round_trip()
{
    uint8_t out[64];
    dfu_protocol::build_info(out, sizeof(out), sample());

    TEST_ASSERT_EQUAL_UINT8(dfu_protocol::INFO_FORMAT, out[dfu_protocol::INFO_OFF_FORMAT]);
    TEST_ASSERT_EQUAL_UINT8(0x7D, out[dfu_protocol::INFO_OFF_MFR]);
    TEST_ASSERT_EQUAL_UINT8(0x01, out[dfu_protocol::INFO_OFF_DEVICE]);
    TEST_ASSERT_EQUAL_UINT8(1, out[dfu_protocol::INFO_OFF_VERSION + 0]);
    TEST_ASSERT_EQUAL_UINT8(2, out[dfu_protocol::INFO_OFF_VERSION + 1]);
    TEST_ASSERT_EQUAL_UINT8(3, out[dfu_protocol::INFO_OFF_VERSION + 2]);
    TEST_ASSERT_EQUAL_UINT8(0, out[dfu_protocol::INFO_OFF_FLAGS]);
}

// The UID includes 0x80 and 0xFF, so this fails if the packing is wrong in either
// direction rather than only for small values.
static void test_uid_round_trips_through_seven_bit_packing()
{
    uint8_t out[64];
    dfu_protocol::build_info(out, sizeof(out), sample());

    uint8_t decoded[12] = {};
    const uint16_t n = sysex_codec::decode_7bit(out + dfu_protocol::INFO_OFF_UID,
                                                decoded, 14u, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT16(12, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(UID, decoded, 12);
}

static void test_app_region_round_trips()
{
    uint8_t out[64];
    dfu_protocol::build_info(out, sizeof(out), sample());

    TEST_ASSERT_EQUAL_UINT32(0x0800C000u,
        dfu_protocol::info_u32(out, dfu_protocol::INFO_OFF_APP_BASE));
    TEST_ASSERT_EQUAL_UINT32(464u * 1024u,
        dfu_protocol::info_u32(out, dfu_protocol::INFO_OFF_APP_SIZE));
}

// A host sizes its chunks from this, so an off-by-one here is an off-by-one in every
// upload that host ever performs.
static void test_chunk_max_round_trips()
{
    uint8_t out[64];
    dfu_protocol::build_info(out, sizeof(out), sample());

    const uint16_t got = (uint16_t)((out[dfu_protocol::INFO_OFF_CHUNK_MAX + 0] << 7)
                                  | out[dfu_protocol::INFO_OFF_CHUNK_MAX + 1]);
    TEST_ASSERT_EQUAL_UINT16(256, got);
}

// The whole 28-bit address space the chunk address can reach must survive the trip,
// not just the addresses this product happens to use.
static void test_full_address_range_round_trips()
{
    const uint32_t cases[] = { 0u, 1u, 0x0FFFFFFFu, 0x08080000u };
    for (uint32_t v : cases) {
        dfu_protocol::Info in = sample();
        in.app_base = v;
        in.app_size = v;

        uint8_t out[64];
        TEST_ASSERT_TRUE(dfu_protocol::build_info(out, sizeof(out), in) > 0);
        TEST_ASSERT_EQUAL_UINT32(v, dfu_protocol::info_u32(out, dfu_protocol::INFO_OFF_APP_BASE));
        TEST_ASSERT_EQUAL_UINT32(v, dfu_protocol::info_u32(out, dfu_protocol::INFO_OFF_APP_SIZE));
    }
}

// Forward compatibility: a reply from a later bootloader carries fields this reader has
// never heard of. Everything it does know must still be readable at its own offset.
static void test_a_longer_reply_still_parses()
{
    uint8_t out[64];
    const uint16_t n = dfu_protocol::build_info(out, sizeof(out), sample());

    // A future bootloader appends four fields we know nothing about.
    for (uint16_t i = n; i < n + 4u; ++i) out[i] = 0x11;

    TEST_ASSERT_EQUAL_UINT8(dfu_protocol::INFO_FORMAT, out[dfu_protocol::INFO_OFF_FORMAT]);
    TEST_ASSERT_EQUAL_UINT8(0x01, out[dfu_protocol::INFO_OFF_DEVICE]);
    TEST_ASSERT_EQUAL_UINT32(0x0800C000u,
        dfu_protocol::info_u32(out, dfu_protocol::INFO_OFF_APP_BASE));
}

// A partial frame would decode to something other than what was meant, so a buffer that
// cannot hold the whole reply must produce nothing at all.
static void test_short_buffer_writes_nothing()
{
    uint8_t out[dfu_protocol::INFO_LEN - 1];
    TEST_ASSERT_EQUAL_UINT16(0, dfu_protocol::build_info(out, sizeof(out), sample()));
}

static void test_null_uid_is_refused()
{
    uint8_t out[64];
    dfu_protocol::Info in = sample();
    in.uid = nullptr;
    TEST_ASSERT_EQUAL_UINT16(0, dfu_protocol::build_info(out, sizeof(out), in));
}

// 0x04 must not collide with anything else the protocol defines, in either direction.
static void test_identify_command_byte_is_distinct()
{
    TEST_ASSERT_EQUAL_UINT8(0x04, dfu_protocol::CMD_IDENTIFY);
    TEST_ASSERT_NOT_EQUAL(dfu_protocol::CMD_IDENTIFY, dfu_protocol::CMD_ENTER_BOOTLOADER);
    TEST_ASSERT_NOT_EQUAL(dfu_protocol::CMD_IDENTIFY, dfu_protocol::CMD_FW_CHUNK);
    TEST_ASSERT_NOT_EQUAL(dfu_protocol::CMD_IDENTIFY, dfu_protocol::CMD_FW_END);
    TEST_ASSERT_NOT_EQUAL(dfu_protocol::CMD_IDENTIFY, dfu_protocol::CMD_FW_BEGIN);
    TEST_ASSERT_NOT_EQUAL(dfu_protocol::CMD_IDENTIFY, dfu_protocol::CMD_FW_STATUS);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_builds_the_declared_length);
    RUN_TEST(test_every_byte_is_seven_bit_safe);
    RUN_TEST(test_scalar_fields_round_trip);
    RUN_TEST(test_uid_round_trips_through_seven_bit_packing);
    RUN_TEST(test_app_region_round_trips);
    RUN_TEST(test_chunk_max_round_trips);
    RUN_TEST(test_full_address_range_round_trips);
    RUN_TEST(test_a_longer_reply_still_parses);
    RUN_TEST(test_short_buffer_writes_nothing);
    RUN_TEST(test_null_uid_is_refused);
    RUN_TEST(test_identify_command_byte_is_distinct);
    return UNITY_END();
}
