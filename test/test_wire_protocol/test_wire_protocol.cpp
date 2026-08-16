// Host-native unit tests for the protocol-3 application wire contract
// (include/pedal_core/wire_protocol.hpp).
//
// Both pedals emit these frames and the host editor parses them, so a mistake
// here is a mistake in two firmwares and one editor at once. The properties
// worth pinning are the ones protocol 3 exists to provide: every byte between
// F0 and F7 stays 7-bit safe, a reader skips a tag it does not know, and a
// frame that outgrows its buffer is dropped rather than truncated and sent.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <pedal_core/wire_protocol.hpp>

using namespace pedal_core::wire;

void setUp(void) {}
void tearDown(void) {}

static bool all_data_bytes_7bit(const uint8_t* f, uint16_t n)
{
    if (n < 2u) return false;
    if (f[0] != SYSEX_START || f[n - 1u] != SYSEX_END) return false;
    for (uint16_t i = 1u; i + 1u < n; ++i)
        if (f[i] & 0x80u) return false;
    return true;
}

// --- Writer ----------------------------------------------------------------

void test_writer_frames_the_envelope(void) {
    uint8_t buf[8] = {};
    Writer w(buf, sizeof(buf));
    w.header(0x01u, cmd::VERSION_REQ);
    w.end();
    TEST_ASSERT_TRUE(w.ok());
    TEST_ASSERT_EQUAL_UINT16(5, w.length());
    TEST_ASSERT_EQUAL_HEX8(0xF0, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7D, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x30, buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0xF7, buf[4]);
}

void test_writer_splits_a_value_into_lo7_hi7(void) {
    uint8_t buf[4] = {};
    Writer w(buf, sizeof(buf));
    w.u14(1023u);
    TEST_ASSERT_EQUAL_UINT8(1023u & 0x7Fu, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(1023u >> 7, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(1023u, read_u14(buf));
}

void test_writer_masks_a_byte_that_would_be_a_status(void) {
    // A raw 0x80+ byte inside a frame aborts it on the receiver, so the writer
    // masks rather than trusting its caller.
    uint8_t buf[4] = {};
    Writer w(buf, sizeof(buf));
    w.u7(0xFFu);
    TEST_ASSERT_EQUAL_HEX8(0x7F, buf[0]);
}

void test_writer_reports_failure_rather_than_truncating(void) {
    // Half a preset frame would restore as garbage, so an overrun must be
    // visible to the caller and not merely shorter.
    uint8_t buf[4] = {};
    Writer w(buf, sizeof(buf));
    w.header(0x01u, cmd::PRESET_DUMP_DATA);
    w.u7(1u);
    w.end();
    TEST_ASSERT_FALSE(w.ok());
    TEST_ASSERT_EQUAL_UINT16(0, w.length());
}

// --- DEVICE_INFO ------------------------------------------------------------

static DeviceInfo sample_info(void)
{
    DeviceInfo d{};
    d.device_id       = 0x01u;
    d.fw_major        = 1u;
    d.fw_minor        = 1u;
    d.fw_patch        = 0u;
    d.slots           = 256u;
    d.algorithm_count = 26u;
    d.param_count     = 16u;
    d.param_bits      = 10u;
    d.name_len        = 16u;
    d.capabilities    = (uint16_t)(cap::EXPRESSION | cap::TEMPO | cap::UID);
    return d;
}

void test_device_info_layout(void) {
    uint8_t buf[32] = {};
    const uint16_t n = build_device_info(sample_info(), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(DEVICE_INFO_FRAME_LEN, n);
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));

    TEST_ASSERT_EQUAL_HEX8(cmd::DEVICE_INFO_DATA, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(PROTOCOL_VERSION, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[7]);
    TEST_ASSERT_EQUAL_UINT16(256u, read_u14(&buf[8]));
    TEST_ASSERT_EQUAL_UINT8(26u, buf[10]);
    TEST_ASSERT_EQUAL_UINT8(16u, buf[11]);
    TEST_ASSERT_EQUAL_UINT8(10u, buf[12]);
    TEST_ASSERT_EQUAL_UINT8(16u, buf[13]);
    TEST_ASSERT_EQUAL_UINT16(sample_info().capabilities, read_u14(&buf[14]));
    TEST_ASSERT_EQUAL_HEX8(SYSEX_END, buf[16]);
}

void test_device_info_carries_a_slot_count_past_128(void) {
    // The bank count left the frame in protocol 3 because it is derivable;
    // what has to survive is the slot count itself, which needs 14 bits.
    DeviceInfo d = sample_info();
    d.slots = 487u;
    uint8_t buf[32] = {};
    build_device_info(d, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(487u, read_u14(&buf[8]));
}

void test_device_info_capability_bits_are_distinct(void) {
    const uint16_t bits[] = { cap::EXPRESSION, cap::TEMPO, cap::NOISE_GLOBAL,
                              cap::EXT_INPUT, cap::BYPASS_FLAG, cap::CALIBRATION,
                              cap::FACTORY_BANK, cap::UID, cap::SAVE_ADDRESSED };
    uint16_t seen = 0u;
    for (const uint16_t b : bits) {
        TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(seen & b));   // no bit reused
        TEST_ASSERT_TRUE(b <= 0x3FFFu);                       // fits the lo7/hi7 pair
        seen = (uint16_t)(seen | b);
    }
}

void test_device_info_refuses_a_short_buffer(void) {
    uint8_t buf[8] = {};
    TEST_ASSERT_EQUAL_UINT16(0, build_device_info(sample_info(), buf, sizeof(buf)));
}

// --- TLV --------------------------------------------------------------------

void test_tlv_round_trips(void) {
    uint8_t buf[32] = {};
    Writer w(buf, sizeof(buf));
    const uint8_t channel[1] = { MIDI_CHANNEL_OMNI };
    const uint8_t noise[3]   = { 1u, 24u, 64u };
    w.tlv(global_tag::CHANNEL, channel, sizeof(channel));
    w.tlv(global_tag::NOISE, noise, sizeof(noise));

    TlvReader r(buf, w.length());
    Tlv t{};
    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::CHANNEL, t.tag);
    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_UINT8(MIDI_CHANNEL_OMNI, t.value[0]);

    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::NOISE, t.tag);
    TEST_ASSERT_EQUAL_UINT8(3, t.len);
    TEST_ASSERT_EQUAL_UINT8(24u, t.value[1]);

    TEST_ASSERT_FALSE(r.next(t));
}

void test_tlv_skips_a_tag_it_does_not_know(void) {
    // The property protocol 3 exists for: a field one product added does not
    // move a byte for a reader that has never heard of it.
    uint8_t buf[32] = {};
    Writer w(buf, sizeof(buf));
    const uint8_t future[4] = { 9u, 9u, 9u, 9u };
    const uint8_t channel[1] = { 3u };
    w.tlv(0x7Au, future, sizeof(future));
    w.tlv(global_tag::CHANNEL, channel, sizeof(channel));

    TlvReader r(buf, w.length());
    Tlv t{};
    TEST_ASSERT_TRUE(r.find(global_tag::CHANNEL, t));
    TEST_ASSERT_EQUAL_UINT8(3u, t.value[0]);
}

void test_tlv_find_restarts_so_order_does_not_matter(void) {
    uint8_t buf[32] = {};
    Writer w(buf, sizeof(buf));
    const uint8_t a[1] = { 7u };
    const uint8_t b[1] = { 1u };
    w.tlv(global_tag::CHANNEL, a, 1u);
    w.tlv(global_tag::BYPASS, b, 1u);

    TlvReader r(buf, w.length());
    Tlv t{};
    TEST_ASSERT_TRUE(r.find(global_tag::BYPASS, t));
    TEST_ASSERT_EQUAL_UINT8(1u, t.value[0]);
    TEST_ASSERT_TRUE(r.find(global_tag::CHANNEL, t));   // still reachable afterwards
    TEST_ASSERT_EQUAL_UINT8(7u, t.value[0]);
}

void test_tlv_stops_at_a_truncated_record(void) {
    // A short tail is what a dropped packet looks like. The records already
    // read stay good; the missing one simply never turns up.
    uint8_t buf[8] = { global_tag::CHANNEL, 1u, 5u, global_tag::NOISE, 3u, 1u };
    TlvReader r(buf, 6u);
    Tlv t{};
    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::CHANNEL, t.tag);
    TEST_ASSERT_FALSE(r.next(t));   // NOISE claims 3 bytes and only 1 is present
}

void test_tlv_absent_tag_is_not_found(void) {
    uint8_t buf[4] = { global_tag::CHANNEL, 1u, 5u, 0u };
    TlvReader r(buf, 3u);
    Tlv t{};
    TEST_ASSERT_FALSE(r.find(global_tag::EXT_INPUT, t));
}

void test_tlv_empty_run_reads_nothing(void) {
    TlvReader r(nullptr, 0u);
    Tlv t{};
    TEST_ASSERT_FALSE(r.next(t));
}

void test_tlv_zero_length_record_is_valid(void) {
    // A flag-shaped tag carries no payload; it must not read as truncation.
    uint8_t buf[4] = { 0x7Bu, 0u, global_tag::CHANNEL, 0u };
    TlvReader r(buf, 2u);
    Tlv t{};
    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(0x7Bu, t.tag);
    TEST_ASSERT_EQUAL_UINT8(0, t.len);
    TEST_ASSERT_FALSE(r.next(t));
}

// --- UID --------------------------------------------------------------------

void test_uid_frame_layout(void) {
    // 12 raw bytes pack to 14 (two groups of 7 -> 8 each, less the trailing
    // group's unused slots): 4 header + 1 version + 14 + 1 end.
    uint8_t packed[14] = {};
    for (uint8_t i = 0; i < sizeof(packed); ++i) packed[i] = (uint8_t)(i + 1u);
    uint8_t buf[32] = {};
    const uint16_t n = build_uid(0x02u, packed, sizeof(packed), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(20, n);
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));
    TEST_ASSERT_EQUAL_HEX8(cmd::UID_DATA, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(PROTOCOL_VERSION, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[5]);
}

// --- constants --------------------------------------------------------------

void test_command_bytes_do_not_collide_with_dfu(void) {
    // The bootloader owns 0x01-0x05 and 0x7E; the application starts at 0x10.
    const uint8_t app[] = { cmd::PRESET_DUMP_REQ, cmd::PRESET_DUMP_DATA, cmd::PRESET_RESTORE,
                            cmd::PRESET_SAVE, cmd::SET_CHANNEL, cmd::FETCH_GLOBAL,
                            cmd::GLOBAL_DATA, cmd::VERSION_REQ, cmd::VERSION_DATA,
                            cmd::DEVICE_INFO_REQ, cmd::DEVICE_INFO_DATA,
                            cmd::UID_REQ, cmd::UID_DATA };
    for (const uint8_t c : app) {
        TEST_ASSERT_TRUE(c >= 0x10u);
        TEST_ASSERT_TRUE(c < 0x7Eu);
        TEST_ASSERT_TRUE(c <= 0x7Fu);   // a command byte is a data byte
    }
}

void test_expression_off_sentinel_is_a_legal_data_byte(void) {
    // The record's 0xFF cannot go on the wire; 0x7F can.
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, EXPR_OFF);
}


// --- the preset frame -------------------------------------------------------

// Build a representative frame: seven parameters, a name, and the two tags a
// multi-effect sends.
static uint16_t build_sample_preset(uint8_t* buf, uint16_t cap, uint8_t device)
{
    PresetHead h{};
    h.slot        = 130u;          // bank 1, program 2
    h.format      = PROTOCOL_VERSION;
    h.algorithm   = 9u;
    h.flags       = 0u;
    h.param_count = 7u;

    Writer w(buf, cap);
    write_preset_head(w, device, cmd::PRESET_DUMP_DATA, h);
    const uint16_t params[7] = { 700u, 0u, 1023u, 512u, 1u, 2u, 930u };
    for (const uint16_t v : params) w.u14(v);

    const char name[] = "Slapback";
    w.u7((uint8_t)(sizeof(name) - 1u));
    w.bytes((const uint8_t*)name, (uint16_t)(sizeof(name) - 1u));

    const uint8_t expr[5]  = { EXPR_OFF, 0u, 0u, 0x7Fu, 0x07u };
    const uint8_t tempo[3] = { 1u, (uint8_t)(1200u & 0x7Fu), (uint8_t)(1200u >> 7) };
    w.tlv(preset_tag::EXPR, expr, sizeof(expr));
    w.tlv(preset_tag::TEMPO, tempo, sizeof(tempo));
    w.end();
    return w.length();
}

void test_preset_round_trips(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));

    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(buf, n, 0x01u, v));
    TEST_ASSERT_EQUAL_UINT16(130u, v.head.slot);
    TEST_ASSERT_EQUAL_UINT8(9u, v.head.algorithm);
    TEST_ASSERT_EQUAL_UINT8(7u, v.head.param_count);
    TEST_ASSERT_EQUAL_UINT16(700u, read_u14(&v.params[0]));
    TEST_ASSERT_EQUAL_UINT16(1023u, read_u14(&v.params[4]));
    TEST_ASSERT_EQUAL_UINT16(930u, read_u14(&v.params[12]));
    TEST_ASSERT_EQUAL_UINT8(8u, v.name_len);
    TEST_ASSERT_EQUAL_UINT8('S', v.name[0]);
    TEST_ASSERT_EQUAL_UINT8('k', v.name[7]);
}

void test_preset_addresses_a_slot_past_128(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[4]);   // bank
    TEST_ASSERT_EQUAL_UINT8(2u, buf[5]);   // program
    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(buf, n, 0x01u, v));
    TEST_ASSERT_EQUAL_UINT16(130u, v.head.slot);
}

void test_preset_tail_carries_the_product_specific_fields(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(buf, n, 0x01u, v));

    TlvReader r(v.tlv, v.tlv_len);
    Tlv t{};
    TEST_ASSERT_TRUE(r.find(preset_tag::TEMPO, t));
    TEST_ASSERT_EQUAL_UINT8(3, t.len);
    TEST_ASSERT_EQUAL_UINT16(1200u, read_u14(&t.value[1]));
    TEST_ASSERT_TRUE(r.find(preset_tag::EXPR, t));
    TEST_ASSERT_EQUAL_UINT8(EXPR_OFF, t.value[0]);
}

void test_preset_with_no_tail_is_valid(void) {
    // What a product without expression or tempo sends. The reader must see an
    // empty tail, not a malformed frame.
    PresetHead h{};
    h.slot = 0u; h.format = PROTOCOL_VERSION; h.algorithm = 0u; h.flags = 0u; h.param_count = 2u;
    uint8_t buf[32] = {};
    Writer w(buf, sizeof(buf));
    write_preset_head(w, 0x02u, cmd::PRESET_DUMP_DATA, h);
    w.u14(1u);
    w.u14(2u);
    w.u7(0u);      // empty name
    w.end();

    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(buf, w.length(), 0x02u, v));
    TEST_ASSERT_EQUAL_UINT8(0, v.name_len);
    TEST_ASSERT_EQUAL_UINT16(0, v.tlv_len);
}

void test_preset_rejects_another_product(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    PresetView v{};
    TEST_ASSERT_FALSE(parse_preset(buf, n, 0x02u, v));
}

void test_preset_rejects_a_parameter_block_running_past_the_end(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    buf[9] = 60u;   // claim sixty parameters
    PresetView v{};
    TEST_ASSERT_FALSE(parse_preset(buf, n, 0x01u, v));
}

void test_preset_rejects_a_name_running_past_the_end(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    buf[4u + PRESET_HEAD_LEN + 14u] = 120u;   // name_len far beyond the frame
    PresetView v{};
    TEST_ASSERT_FALSE(parse_preset(buf, n, 0x01u, v));
}

void test_preset_rejects_a_truncated_frame(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    PresetView v{};
    for (uint16_t cut = 1u; cut < n; ++cut) {
        // Any prefix is missing its F7, so none of them may parse.
        TEST_ASSERT_FALSE(parse_preset(buf, cut, 0x01u, v));
    }
}

void test_preset_rejects_a_foreign_manufacturer(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    buf[1] = 0x41u;   // Roland
    PresetView v{};
    TEST_ASSERT_FALSE(parse_preset(buf, n, 0x01u, v));
}

void test_preset_rejects_an_unrelated_command(void) {
    uint8_t buf[96] = {};
    const uint16_t n = build_sample_preset(buf, sizeof(buf), 0x01u);
    buf[3] = cmd::GLOBAL_DATA;
    PresetView v{};
    TEST_ASSERT_FALSE(parse_preset(buf, n, 0x01u, v));
}


// --- addressing by unit -----------------------------------------------------
//
// A pedal must reboot into DFU only when the command names it. Over DIN every
// pedal downstream sees every byte and two of a model share a device byte, so
// an unaddressed reboot would take a whole chain down at once.

static void pack_uid(const uint8_t* uid, uint8_t* out)
{
    sysex_codec::encode_7bit(uid, out, UID_BYTE_LEN, UID_PACKED_LEN);
}

void test_uid_matches_its_own(void) {
    uint8_t uid[UID_BYTE_LEN];
    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) uid[i] = (uint8_t)(0x11u * (i + 1u));
    uint8_t packed[UID_PACKED_LEN] = {};
    pack_uid(uid, packed);
    TEST_ASSERT_TRUE(uid_matches(packed, UID_PACKED_LEN, uid, UID_BYTE_LEN));
}

void test_uid_rejects_another_unit(void) {
    uint8_t mine[UID_BYTE_LEN];
    uint8_t theirs[UID_BYTE_LEN];
    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) {
        mine[i]   = (uint8_t)(0x11u * (i + 1u));
        theirs[i] = mine[i];
    }
    theirs[UID_BYTE_LEN - 1u] ^= 0x01u;   // one bit apart: the next unit off the line
    uint8_t packed[UID_PACKED_LEN] = {};
    pack_uid(theirs, packed);
    TEST_ASSERT_FALSE(uid_matches(packed, UID_PACKED_LEN, mine, UID_BYTE_LEN));
}

void test_uid_rejects_a_high_byte_difference(void) {
    // The 7-bit packing carries bit 7 in a separate byte, so a difference only
    // in the high bits must survive the round trip and still be seen.
    uint8_t mine[UID_BYTE_LEN] = {};
    uint8_t theirs[UID_BYTE_LEN] = {};
    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) { mine[i] = 0x80u; theirs[i] = 0x00u; }
    uint8_t packed[UID_PACKED_LEN] = {};
    pack_uid(theirs, packed);
    TEST_ASSERT_FALSE(uid_matches(packed, UID_PACKED_LEN, mine, UID_BYTE_LEN));
    pack_uid(mine, packed);
    TEST_ASSERT_TRUE(uid_matches(packed, UID_PACKED_LEN, mine, UID_BYTE_LEN));
}

void test_uid_rejects_an_unaddressed_command(void) {
    // The whole point: a bare reboot command names no unit, so no unit obeys it.
    uint8_t uid[UID_BYTE_LEN];
    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) uid[i] = (uint8_t)(i + 1u);
    TEST_ASSERT_FALSE(uid_matches(nullptr, 0u, uid, UID_BYTE_LEN));
}

void test_uid_rejects_a_truncated_payload(void) {
    uint8_t uid[UID_BYTE_LEN];
    for (uint8_t i = 0; i < UID_BYTE_LEN; ++i) uid[i] = (uint8_t)(i + 1u);
    uint8_t packed[UID_PACKED_LEN] = {};
    pack_uid(uid, packed);
    for (uint8_t len = 0; len < UID_PACKED_LEN; ++len) {
        TEST_ASSERT_FALSE(uid_matches(packed, len, uid, UID_BYTE_LEN));
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_writer_frames_the_envelope);
    RUN_TEST(test_writer_splits_a_value_into_lo7_hi7);
    RUN_TEST(test_writer_masks_a_byte_that_would_be_a_status);
    RUN_TEST(test_writer_reports_failure_rather_than_truncating);
    RUN_TEST(test_device_info_layout);
    RUN_TEST(test_device_info_carries_a_slot_count_past_128);
    RUN_TEST(test_device_info_capability_bits_are_distinct);
    RUN_TEST(test_device_info_refuses_a_short_buffer);
    RUN_TEST(test_tlv_round_trips);
    RUN_TEST(test_tlv_skips_a_tag_it_does_not_know);
    RUN_TEST(test_tlv_find_restarts_so_order_does_not_matter);
    RUN_TEST(test_tlv_stops_at_a_truncated_record);
    RUN_TEST(test_tlv_absent_tag_is_not_found);
    RUN_TEST(test_tlv_empty_run_reads_nothing);
    RUN_TEST(test_tlv_zero_length_record_is_valid);
    RUN_TEST(test_uid_frame_layout);
    RUN_TEST(test_command_bytes_do_not_collide_with_dfu);
    RUN_TEST(test_expression_off_sentinel_is_a_legal_data_byte);
    RUN_TEST(test_preset_round_trips);
    RUN_TEST(test_preset_addresses_a_slot_past_128);
    RUN_TEST(test_preset_tail_carries_the_product_specific_fields);
    RUN_TEST(test_preset_with_no_tail_is_valid);
    RUN_TEST(test_preset_rejects_another_product);
    RUN_TEST(test_preset_rejects_a_parameter_block_running_past_the_end);
    RUN_TEST(test_preset_rejects_a_name_running_past_the_end);
    RUN_TEST(test_preset_rejects_a_truncated_frame);
    RUN_TEST(test_preset_rejects_a_foreign_manufacturer);
    RUN_TEST(test_preset_rejects_an_unrelated_command);
    RUN_TEST(test_uid_matches_its_own);
    RUN_TEST(test_uid_rejects_another_unit);
    RUN_TEST(test_uid_rejects_a_high_byte_difference);
    RUN_TEST(test_uid_rejects_an_unaddressed_command);
    RUN_TEST(test_uid_rejects_a_truncated_payload);
    return UNITY_END();
}
