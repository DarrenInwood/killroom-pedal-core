// The DFU write session against a RAM-backed flash fake.
//
// This is the code that bricks a pedal when it is wrong: every bounds check
// below stands between a malformed SysEx frame and a half-written application.
// In the donor firmware this logic lived inside a bootloader main.cpp bound to
// CMSIS and TinyUSB, where none of it could be exercised on a host.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include <pedal_core/dfu_session.hpp>
#include <pedal_core/sysex_codec.hpp>

using dfu_protocol::Status;

// --- The flash fake ----------------------------------------------------------
static constexpr uint32_t REGION    = 4096u;
static constexpr uint32_t FAKE_BASE = 0x08004000u;   // where the app region lives on target
static uint8_t  g_flash[REGION];
static uint32_t g_prepare_calls = 0;
static uint32_t g_write_calls = 0;
static bool     g_prepare_ok = true;
static bool     g_write_ok = true;

static bool fake_prepare(uint32_t addr, uint32_t len, void*)
{
    ++g_prepare_calls;
    if (!g_prepare_ok) return false;
    (void)addr; (void)len;
    return true;
}

static bool fake_write(uint32_t addr, const uint8_t* data, uint32_t len, void*)
{
    ++g_write_calls;
    if (!g_write_ok) return false;
    memcpy(&g_flash[addr - FAKE_BASE], data, len);
    return true;
}

// The region as the CPU reads it. On target this is the base cast to a
// pointer; here it is the array, which is the whole reason the seam exists.
static const uint8_t* fake_mapped(void*) { return g_flash; }

static dfu::Session s;

static void begin_session(void)
{
    const dfu::FlashOps ops = { fake_prepare, fake_write, fake_mapped, nullptr };
    s.begin(FAKE_BASE, REGION, ops);
}

void setUp(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_prepare_calls = g_write_calls = 0;
    g_prepare_ok = g_write_ok = true;
    begin_session();
}
void tearDown(void) {}

// --- Frame builders ----------------------------------------------------------
// Roland-style packing: one MSB byte then up to seven data bytes.
static std::vector<uint8_t> pack7(const uint8_t* data, uint16_t len)
{
    std::vector<uint8_t> out;
    for (uint16_t i = 0; i < len; i += 7u) {
        const uint16_t n = (uint16_t)((len - i) < 7u ? (len - i) : 7u);
        uint8_t msbs = 0;
        for (uint16_t j = 0; j < n; ++j)
            if (data[i + j] & 0x80u) msbs = (uint8_t)(msbs | (1u << j));
        out.push_back(msbs);
        for (uint16_t j = 0; j < n; ++j) out.push_back((uint8_t)(data[i + j] & 0x7Fu));
    }
    return out;
}

static std::vector<uint8_t> begin_payload(uint32_t size)
{
    const uint8_t sz[4] = { (uint8_t)size, (uint8_t)(size >> 8),
                            (uint8_t)(size >> 16), (uint8_t)(size >> 24) };
    return pack7(sz, 4);
}

static std::vector<uint8_t> chunk_payload(uint32_t addr, const uint8_t* data, uint16_t len)
{
    std::vector<uint8_t> p = {
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
        (uint8_t)(len >> 8), (uint8_t)len,
    };
    const std::vector<uint8_t> enc = pack7(data, len);
    p.insert(p.end(), enc.begin(), enc.end());
    return p;
}

static Status feed(uint8_t cmd, const std::vector<uint8_t>& payload, uint32_t& addr)
{
    return s.handle(cmd, payload.data(), (uint16_t)payload.size(), addr);
}

// --- Happy path --------------------------------------------------------------

void test_begin_then_chunk_then_verify(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;

    uint8_t image[512];
    for (uint16_t i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 7u + 3u);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Ack,
                            (uint8_t)feed(dfu_protocol::CMD_FW_BEGIN,
                                          begin_payload(sizeof(image)), addr));
    TEST_ASSERT_EQUAL_UINT32(base, addr);
    TEST_ASSERT_EQUAL_UINT32(sizeof(image), s.total());

    for (uint16_t off = 0; off < sizeof(image); off += 256u) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Ack,
            (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK,
                          chunk_payload(base + off, &image[off], 256u), addr));
        TEST_ASSERT_EQUAL_UINT32(base + off + 256u, addr);   // resume point
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof(image), s.received());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(image, g_flash, sizeof(image));

    // High bits survive the 7-bit round trip — the whole point of the packing.
    const uint32_t crc = sysex_codec::crc32_update(0, image, sizeof(image));
    const uint8_t c[4] = { (uint8_t)crc, (uint8_t)(crc >> 8),
                           (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Complete,
                            (uint8_t)feed(dfu_protocol::CMD_FW_END, pack7(c, 4), addr));
}

void test_wrong_crc_reports_crcfail_not_complete(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t image[64];
    memset(image, 0xA5, sizeof(image));
    feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(sizeof(image)), addr);
    feed(dfu_protocol::CMD_FW_CHUNK, chunk_payload(base, image, sizeof(image)), addr);
    const uint8_t bad[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::CrcFail,
                            (uint8_t)feed(dfu_protocol::CMD_FW_END, pack7(bad, 4), addr));
}

// --- The rejections ----------------------------------------------------------

void test_chunk_past_the_region_is_rejected(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[64] = {};
    feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(REGION), addr);
    // Starts inside, ends past the end: the check must be on the END of the write.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK,
                      chunk_payload(base + REGION - 32u, data, sizeof(data)), addr));
    TEST_ASSERT_EQUAL_UINT32(0u, g_write_calls);
}

void test_chunk_below_the_region_is_rejected(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[8] = {};
    // The bootloader's own flash lives below the app region: a chunk aimed
    // there would erase the updater mid-update.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK,
                      chunk_payload(base - 16u, data, sizeof(data)), addr));
    TEST_ASSERT_EQUAL_UINT32(0u, g_write_calls);
}

void test_unaligned_address_is_rejected(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[8] = {};
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK,
                      chunk_payload(base + 2u, data, sizeof(data)), addr));
}

void test_length_longer_than_the_payload_is_rejected(void) {
    // A frame claiming 256 bytes but carrying 8: decoding what is there would
    // write stale buffer bytes into flash.
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::vector<uint8_t> p = chunk_payload(base, data, sizeof(data));
    p[4] = 0x01; p[5] = 0x00;                       // claim 256
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
                            (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK, p, addr));
    TEST_ASSERT_EQUAL_UINT32(0u, g_write_calls);
}

void test_oversized_chunk_is_rejected(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[8] = {};
    std::vector<uint8_t> p = chunk_payload(base, data, sizeof(data));
    p[4] = 0x02; p[5] = 0x00;                       // 512 > CHUNK_MAX
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
                            (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK, p, addr));
}

void test_short_and_unknown_frames_are_rejected(void) {
    uint32_t addr = 0;
    const std::vector<uint8_t> tiny = { 0x00, 0x01 };
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
                            (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK, tiny, addr));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
                            (uint8_t)feed(0x42u, tiny, addr));
}

void test_begin_larger_than_the_region_is_rejected(void) {
    uint32_t addr = 0;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(REGION + 4u), addr));
    TEST_ASSERT_EQUAL_UINT32(0u, s.total());
}

void test_flash_failure_nacks_and_does_not_advance(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[64] = {};
    feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(256u), addr);

    g_write_ok = false;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK, chunk_payload(base, data, sizeof(data)), addr));
    TEST_ASSERT_EQUAL_UINT32(base, addr);       // resume point unmoved
    TEST_ASSERT_EQUAL_UINT32(0u, s.received());

    // A failed erase is equally fatal to the chunk, and never reaches write().
    g_write_ok = true;
    g_prepare_ok = false;
    const uint32_t writes = g_write_calls;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Nack,
        (uint8_t)feed(dfu_protocol::CMD_FW_CHUNK, chunk_payload(base, data, sizeof(data)), addr));
    TEST_ASSERT_EQUAL_UINT32(writes, g_write_calls);
}

void test_resending_begin_restarts_cleanly(void) {
    uint32_t addr = 0;
    const uint32_t base = FAKE_BASE;
    uint8_t data[128] = {};
    feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(512u), addr);
    feed(dfu_protocol::CMD_FW_CHUNK, chunk_payload(base, data, sizeof(data)), addr);
    TEST_ASSERT_EQUAL_UINT32(128u, s.received());

    // The documented retry: the host resends FW_BEGIN and starts over.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Status::Ack,
        (uint8_t)feed(dfu_protocol::CMD_FW_BEGIN, begin_payload(512u), addr));
    TEST_ASSERT_EQUAL_UINT32(0u, s.received());
    TEST_ASSERT_EQUAL_UINT32(base, addr);
    TEST_ASSERT_EQUAL_UINT32(512u, s.total());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_then_chunk_then_verify);
    RUN_TEST(test_wrong_crc_reports_crcfail_not_complete);
    RUN_TEST(test_chunk_past_the_region_is_rejected);
    RUN_TEST(test_chunk_below_the_region_is_rejected);
    RUN_TEST(test_unaligned_address_is_rejected);
    RUN_TEST(test_length_longer_than_the_payload_is_rejected);
    RUN_TEST(test_oversized_chunk_is_rejected);
    RUN_TEST(test_short_and_unknown_frames_are_rejected);
    RUN_TEST(test_begin_larger_than_the_region_is_rejected);
    RUN_TEST(test_flash_failure_nacks_and_does_not_advance);
    RUN_TEST(test_resending_begin_restarts_cleanly);
    return UNITY_END();
}
