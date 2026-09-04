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
#include <initializer_list>
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

// Every bit, not most of them: the ones most likely to collide are the ones appended last,
// and those were the five this list used to stop short of.
void test_device_info_capability_bits_are_distinct(void) {
    const uint16_t bits[] = { cap::EXPRESSION, cap::TEMPO, cap::RETIRED_NOISE_GLOBAL,
                              cap::EXT_INPUT, cap::BYPASS_FLAG, cap::CALIBRATION,
                              cap::FACTORY_BANK, cap::UID, cap::SAVE_ADDRESSED,
                              cap::BOOST, cap::MIDI_ROUTING, cap::PRESET_EXTRAS,
                              cap::SCENE_LATCH, cap::TEMPO_DIVISION };
    uint16_t seen = 0u;
    for (const uint16_t b : bits) {
        TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(seen & b));   // no bit reused
        TEST_ASSERT_TRUE(b <= 0x3FFFu);                       // fits the lo7/hi7 pair
        seen = (uint16_t)(seen | b);
    }
    // Fourteen bits, and the field is full: a fifteenth needs DEVICE_INFO's field widened,
    // which is a wire change rather than a constant.
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x3FFFu, seen, "the capability field is not full");
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
    const uint8_t ext[4]     = { 1u, 24u, 64u, 3u };
    w.tlv(global_tag::CHANNEL, channel, sizeof(channel));
    w.tlv(global_tag::EXT_INPUT, ext, sizeof(ext));

    TlvReader r(buf, w.length());
    Tlv t{};
    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::CHANNEL, t.tag);
    TEST_ASSERT_EQUAL_UINT8(1, t.len);
    TEST_ASSERT_EQUAL_UINT8(MIDI_CHANNEL_OMNI, t.value[0]);

    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::EXT_INPUT, t.tag);
    TEST_ASSERT_EQUAL_UINT8(4, t.len);
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
    uint8_t buf[8] = { global_tag::CHANNEL, 1u, 5u, global_tag::EXT_INPUT, 4u, 1u };
    TlvReader r(buf, 6u);
    Tlv t{};
    TEST_ASSERT_TRUE(r.next(t));
    TEST_ASSERT_EQUAL_HEX8(global_tag::CHANNEL, t.tag);
    TEST_ASSERT_FALSE(r.next(t));   // EXT_INPUT claims 4 bytes and only 1 is present
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

// A representative frame: seven parameters, a name, and the two tags a multi-effect sends.
//
// Built through the library's own builder rather than laid out here. A suite that
// reimplements the layout it is testing agrees with whatever it copied, which is what this
// one did while build_preset did not exist.
static const uint16_t SAMPLE_PARAMS[7] = { 700u, 0u, 1023u, 512u, 1u, 2u, 930u };
static const char     SAMPLE_NAME[]    = "Slapback";

static PresetHead sample_preset_head()
{
    PresetHead h{};
    h.slot        = 130u;          // bank 1, program 2
    h.format      = PROTOCOL_VERSION;
    h.algorithm   = 9u;
    h.flags       = 0u;
    h.param_count = 7u;
    return h;
}

// The TLV run the sample carries, as the bytes it is on the wire.
static uint16_t sample_preset_tlv(uint8_t* out, uint16_t cap)
{
    const uint8_t expr[5]  = { EXPR_OFF, 0u, 0u, 0x7Fu, 0x07u };
    const uint8_t tempo[3] = { 1u, (uint8_t)(1200u & 0x7Fu), (uint8_t)(1200u >> 7) };
    uint16_t n = 0u;
    const auto put = [&](uint8_t tag, const uint8_t* v, uint8_t len) {
        if ((uint16_t)(n + 2u + len) > cap) return;
        out[n++] = tag;
        out[n++] = len;
        for (uint8_t i = 0; i < len; ++i) out[n++] = v[i];
    };
    put(preset_tag::EXPR, expr, (uint8_t)sizeof(expr));
    put(preset_tag::TEMPO, tempo, (uint8_t)sizeof(tempo));
    return n;
}

static uint16_t build_sample_preset(uint8_t* buf, uint16_t cap, uint8_t device)
{
    uint8_t tlv[16] = {};
    PresetBody b{};
    b.params   = SAMPLE_PARAMS;
    b.name     = SAMPLE_NAME;
    b.name_len = (uint8_t)(sizeof(SAMPLE_NAME) - 1u);
    b.tlv      = tlv;
    b.tlv_len  = sample_preset_tlv(tlv, sizeof(tlv));
    return build_preset(sample_preset_head(), b, device, cmd::PRESET_DUMP_DATA, buf, cap);
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

// --- the preset builder -----------------------------------------------------

// Build, parse, build again: the same bytes both times.
//
// This is the shape a round-trip cannot manage. A parse of a build proves the two agree
// with each other; rebuilding from what the parser produced proves the frame is the only
// one that describes that preset, so a field that moved on one side and not the other
// stops being invisible.
void test_the_preset_frame_is_a_fixed_point(void) {
    uint8_t first[96] = {};
    const uint16_t n1 = build_sample_preset(first, sizeof(first), 0x01u);
    TEST_ASSERT_TRUE(n1 > 0);

    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(first, n1, 0x01u, v));

    // Everything the second build needs comes from the parse, not from the sample.
    uint16_t params[16] = {};
    for (uint8_t i = 0; i < v.head.param_count; ++i) params[i] = read_u14(&v.params[i * 2u]);

    PresetBody b{};
    b.params   = params;
    b.name     = (const char*)v.name;
    b.name_len = v.name_len;
    b.tlv      = v.tlv;
    b.tlv_len  = v.tlv_len;

    uint8_t second[96] = {};
    const uint16_t n2 = build_preset(v.head, b, 0x01u, cmd::PRESET_DUMP_DATA,
                                     second, sizeof(second));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(n1, n2, "the rebuilt frame was a different length");
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(first, second, n1, "the rebuilt frame differs");
}

// The head owns the count, so a block that disagrees with it cannot be described. What used
// to make parse_preset read the name out of the parameter block is now unsayable.
void test_the_builder_writes_exactly_the_count_the_head_gives(void) {
    PresetHead h = sample_preset_head();
    h.param_count = 3u;

    PresetBody b{};
    b.params   = SAMPLE_PARAMS;          // still seven values; only three are the frame's
    b.name     = SAMPLE_NAME;
    b.name_len = (uint8_t)(sizeof(SAMPLE_NAME) - 1u);

    uint8_t buf[96] = {};
    const uint16_t n = build_preset(h, b, 0x01u, cmd::PRESET_DUMP_DATA, buf, sizeof(buf));

    PresetView v{};
    TEST_ASSERT_TRUE(parse_preset(buf, n, 0x01u, v));
    TEST_ASSERT_EQUAL_UINT8(3u, v.head.param_count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(b.name_len, v.name_len, "the name was read from the wrong offset");
    for (uint8_t i = 0; i < 3u; ++i)
        TEST_ASSERT_EQUAL_UINT16(SAMPLE_PARAMS[i], read_u14(&v.params[i * 2u]));
}

// A part promised and not supplied is a caller bug rather than a short frame, and is said
// so rather than written as a block of zeros a restore would believe.
void test_the_builder_refuses_a_part_it_was_not_given(void) {
    uint8_t buf[96] = {};
    PresetHead h = sample_preset_head();

    PresetBody no_params{};
    no_params.name     = SAMPLE_NAME;
    no_params.name_len = 1u;
    TEST_ASSERT_EQUAL_UINT16(0u, build_preset(h, no_params, 0x01u, cmd::PRESET_DUMP_DATA,
                                              buf, sizeof(buf)));

    PresetBody no_name{};
    no_name.params   = SAMPLE_PARAMS;
    no_name.name_len = 4u;               // promised four bytes, handed none
    TEST_ASSERT_EQUAL_UINT16(0u, build_preset(h, no_name, 0x01u, cmd::PRESET_DUMP_DATA,
                                              buf, sizeof(buf)));
}

// Only the two commands parse_preset accepts, so a frame this builds is one it reads.
void test_the_builder_refuses_a_command_the_parser_would_not_take(void) {
    uint8_t buf[96] = {};
    PresetBody b{};
    b.params = SAMPLE_PARAMS;
    TEST_ASSERT_EQUAL_UINT16(0u, build_preset(sample_preset_head(), b, 0x01u,
                                              cmd::GLOBAL_DATA, buf, sizeof(buf)));
}

// Drops rather than truncates, like everything else Writer builds: half a preset frame
// restores as garbage.
void test_the_builder_refuses_a_short_buffer(void) {
    uint8_t small[12] = {};
    TEST_ASSERT_EQUAL_UINT16(0u, build_sample_preset(small, sizeof(small), 0x01u));
}

// The advertised maximum really does hold what the builder produces.
void test_a_built_preset_fits_the_advertised_maximum(void) {
    uint8_t tlv[16] = {};
    PresetBody b{};
    b.params   = SAMPLE_PARAMS;
    b.name     = SAMPLE_NAME;
    b.name_len = (uint8_t)(sizeof(SAMPLE_NAME) - 1u);
    b.tlv      = tlv;
    b.tlv_len  = sample_preset_tlv(tlv, sizeof(tlv));

    const PresetHead h = sample_preset_head();
    const uint16_t bound = preset_frame_max(h.param_count, b.name_len, (uint8_t)b.tlv_len);

    uint8_t buf[96] = {};
    const uint16_t n = build_preset(h, b, 0x01u, cmd::PRESET_DUMP_DATA, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE_MESSAGE(n <= bound, "the frame outgrew the bound a product sizes from");
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));
}

// The slot is a bank and a program on the wire, most significant seven bits first. The
// split and the join are one spelling each, and this is the pair holding them together.
void test_the_slot_pair_round_trips(void) {
    for (uint16_t slot : { (uint16_t)0u, (uint16_t)1u, (uint16_t)127u,
                           (uint16_t)128u, (uint16_t)130u, (uint16_t)16383u }) {
        TEST_ASSERT_EQUAL_UINT16(slot, slot_of(slot_bank(slot), slot_program(slot)));
    }
    TEST_ASSERT_EQUAL_UINT8(1u, slot_bank(130u));
    TEST_ASSERT_EQUAL_UINT8(2u, slot_program(130u));
}


// --- addressing by unit -----------------------------------------------------
//
// A pedal must reboot into DFU only when the command names it. Over the jack every
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


// --- the global frame ------------------------------------------------------

// A GlobalView with every field present and nothing left at its default, so a field that
// fails to survive the round trip shows up as itself rather than as a coincidence.
static GlobalView sample_global(void) {
    GlobalView g{};
    g.channel = 9u;                     g.has_channel = true;
    g.ext_input.mode = 1u;
    g.ext_input.press[0] = 4u; g.ext_input.press[1] = 3u; g.ext_input.press[2] = 2u;
    g.ext_input.hold[0]  = 8u; g.ext_input.hold[1]  = 13u; g.ext_input.hold[2] = 11u;
    g.ext_input.has_holds = true;       g.has_ext_input = true;
    g.bypass_active = false;            g.has_bypass = true;
    g.scene_active = 1u;                g.has_scene_active = true;

    g.routing.rx_channel = 5u;
    g.routing.omni = true;
    g.routing.tx_channel = 11u;
    g.routing.out = midi_routing::out_mode::THRU;
    g.routing.pc_offset = 1u;
    g.routing.clock_out = true;
    g.routing.clock_thru = false;
    g.routing.usb_jack_route = midi_routing::usb_jack::BOTH;
    g.routing.rx_pc = false;
    g.routing.rx_sysex = false;
    g.routing.tx_params = false;
    g.routing.tx = midi_routing::tx_state::PC_BYPASS;
    g.has_routing = true;
    return g;
}

void test_global_round_trips(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const GlobalView sent = sample_global();
    const uint16_t n = build_global(sent, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));

    TEST_ASSERT_TRUE(got.has_channel);      TEST_ASSERT_EQUAL_UINT8(9u, got.channel);
    TEST_ASSERT_TRUE(got.has_ext_input);
    TEST_ASSERT_EQUAL_UINT8(1u, got.ext_input.mode);
    TEST_ASSERT_EQUAL_UINT8(4u, got.ext_input.press[0]);
    TEST_ASSERT_EQUAL_UINT8(2u, got.ext_input.press[2]);
    TEST_ASSERT_TRUE(got.ext_input.has_holds);
    TEST_ASSERT_EQUAL_UINT8(8u,  got.ext_input.hold[0]);
    TEST_ASSERT_EQUAL_UINT8(11u, got.ext_input.hold[2]);
    TEST_ASSERT_TRUE(got.has_bypass);       TEST_ASSERT_FALSE(got.bypass_active);
    TEST_ASSERT_TRUE(got.has_scene_active); TEST_ASSERT_EQUAL_UINT8(1u, got.scene_active);
}

// All twelve bytes of the routing block survive the trip. The block is the one record a
// host writes back exactly as it read it, so a field that silently reverts is a setting
// the player cannot change.
void test_global_routing_block_round_trips_every_field(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(sample_global(), 0x01u, buf, sizeof(buf));
    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_TRUE(got.has_routing);

    TEST_ASSERT_EQUAL_UINT8(5u,  got.routing.rx_channel);
    TEST_ASSERT_TRUE(got.routing.omni);
    TEST_ASSERT_EQUAL_UINT8(11u, got.routing.tx_channel);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::out_mode::THRU, got.routing.out);
    TEST_ASSERT_EQUAL_UINT8(1u,  got.routing.pc_offset);
    TEST_ASSERT_TRUE(got.routing.clock_out);
    TEST_ASSERT_FALSE(got.routing.clock_thru);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::usb_jack::BOTH, got.routing.usb_jack_route);
    TEST_ASSERT_FALSE(got.routing.rx_pc);
    TEST_ASSERT_FALSE(got.routing.rx_sysex);
    TEST_ASSERT_FALSE(got.routing.tx_params);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::tx_state::PC_BYPASS, got.routing.tx);
}

// The routing record, byte by byte where it sits in the frame.
//
// Every other test of this block round-trips it, which proves the encoder and the decoder
// agree with each other and nothing about what either agrees with. Swap two entries in
// midi_routing's offset list and a round-trip stays green while every shipped host editor
// misparses. These are the positions the wire contract fixes, so they are written out
// rather than derived.
void test_the_routing_record_sits_where_the_wire_says(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(sample_global(), 0x01u, buf, sizeof(buf));

    // Find the record rather than assuming where the preceding ones ended: this test is
    // about the payload's own layout, not about the order the encoder writes records in.
    uint16_t i = 4u;                     // past F0 <mfr> <dev> <cmd>
    while (i + 1u < n && buf[i] != global_tag::MIDI_ROUTING) i = (uint16_t)(i + 2u + buf[i + 1u]);
    TEST_ASSERT_TRUE_MESSAGE(i + 1u < n, "the routing record is not in the frame");
    TEST_ASSERT_EQUAL_UINT8(midi_routing::LEN, buf[i + 1u]);

    const uint8_t* p = &buf[i + 2u];
    TEST_ASSERT_EQUAL_UINT8(5u,  p[midi_routing::RX_CHANNEL]);
    TEST_ASSERT_EQUAL_UINT8(1u,  p[midi_routing::OMNI]);
    TEST_ASSERT_EQUAL_UINT8(11u, p[midi_routing::TX_CHANNEL]);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::out_mode::THRU, p[midi_routing::OUT_MODE]);
    TEST_ASSERT_EQUAL_UINT8(1u,  p[midi_routing::PC_OFFSET]);
    TEST_ASSERT_EQUAL_UINT8(1u,  p[midi_routing::CLOCK_OUT]);
    TEST_ASSERT_EQUAL_UINT8(0u,  p[midi_routing::CLOCK_THRU]);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::usb_jack::BOTH, p[midi_routing::USB_JACK]);
    TEST_ASSERT_EQUAL_UINT8(0u,  p[midi_routing::RX_PC]);
    TEST_ASSERT_EQUAL_UINT8(0u,  p[midi_routing::RX_SYSEX]);
    TEST_ASSERT_EQUAL_UINT8(0u,  p[midi_routing::TX_PARAMS]);
    TEST_ASSERT_EQUAL_UINT8(midi_routing::tx_state::PC_BYPASS, p[midi_routing::TX_STATE]);
}

// The follow-the-receive-channel sentinel is 0x7F on the wire, because 0xFF is not a
// legal SysEx data byte. It has to survive as itself rather than as channel 127.
void test_global_carries_the_follow_sentinel(void) {
    GlobalView g{};
    g.has_routing = true;                       // a default block already means follow
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(g, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_EQUAL_UINT8(midi_routing::TX_CHANNEL_FOLLOW_RX, got.routing.tx_channel);
}

// What a product does not have, it does not send, and what a frame does not carry is a
// question the pedal was not asked rather than a setting turned off.
void test_global_omits_the_fields_a_product_does_not_have(void) {
    GlobalView g{};
    g.channel = 3u; g.has_channel = true;       // a pedal with nothing else to report

    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(g, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(5u + 2u + 1u, n);  // the envelope and one record

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_TRUE(got.has_channel);
    TEST_ASSERT_FALSE(got.has_ext_input);
    TEST_ASSERT_FALSE(got.has_bypass);
    TEST_ASSERT_FALSE(got.has_routing);
    TEST_ASSERT_FALSE(got.has_scene_active);
}

// A jack that predates the hold assignments sends four bytes, and a reader that knows
// about them reads the shorter record without inventing three actions.
void test_global_ext_input_without_the_hold_tail(void) {
    GlobalView g{};
    g.ext_input.mode = 1u;
    g.ext_input.press[0] = 4u; g.ext_input.press[1] = 3u; g.ext_input.press[2] = 2u;
    g.ext_input.hold[0] = 9u;                   // set, but not sent
    g.ext_input.has_holds = false;
    g.has_ext_input = true;

    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(g, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(5u + 2u + 4u, n);

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_TRUE(got.has_ext_input);
    TEST_ASSERT_FALSE(got.ext_input.has_holds);
    TEST_ASSERT_EQUAL_UINT8(4u, got.ext_input.press[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, got.ext_input.hold[0]);
}

// An empty frame is valid: a pedal with nothing to report still answers the query.
void test_global_with_no_records_is_valid(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(GlobalView{}, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT16(5u, n);
    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_FALSE(got.has_channel);
}

// A tag a firmware does not know is skipped by length, and the records around it still
// read: the property protocol 3 exists to provide.
void test_global_skips_a_tag_it_does_not_know(void) {
    uint8_t buf[GLOBAL_FRAME_MAX + 8u] = {};
    Writer w(buf, sizeof(buf));
    w.header(0x01u, cmd::GLOBAL_DATA);
    const uint8_t future[3] = { 1u, 2u, 3u };
    w.tlv(0x7Au, future, 3u);                   // a tag from a later firmware
    const uint8_t ch = 7u;
    w.tlv(global_tag::CHANNEL, &ch, 1u);
    w.end();

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, w.length(), 0x01u, got));
    TEST_ASSERT_TRUE(got.has_channel);
    TEST_ASSERT_EQUAL_UINT8(7u, got.channel);
}

// A record shorter than the field it names is left absent rather than half-believed.
void test_global_leaves_a_short_record_absent(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    Writer w(buf, sizeof(buf));
    w.header(0x01u, cmd::GLOBAL_DATA);
    const uint8_t half[3] = { 1u, 40u, 2u };    // EXT_INPUT wants four
    w.tlv(global_tag::EXT_INPUT, half, 3u);
    w.end();

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, w.length(), 0x01u, got));
    TEST_ASSERT_FALSE(got.has_ext_input);
}

// A retired tag is skipped by its length, not choked on. A pedal running firmware that
// predates the retirement still sends 0x11, and everything after it in the same frame has
// to survive that.
void test_global_skips_a_retired_record(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    Writer w(buf, sizeof(buf));
    w.header(0x01u, cmd::GLOBAL_DATA);
    const uint8_t stale[3] = { 1u, 40u, 77u };
    w.tlv(global_tag::RETIRED_NOISE, stale, 3u);
    const uint8_t channel[1] = { 7u };
    w.tlv(global_tag::CHANNEL, channel, 1u);
    w.end();

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, w.length(), 0x01u, got));
    TEST_ASSERT_TRUE(got.has_channel);          // the record behind it still arrived
    TEST_ASSERT_EQUAL_UINT8(7u, got.channel);
}

void test_global_rejects_another_product(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(sample_global(), 0x01u, buf, sizeof(buf));
    GlobalView got{};
    TEST_ASSERT_FALSE(parse_global(buf, n, 0x02u, got));
}

void test_global_rejects_an_unrelated_command(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(sample_global(), 0x01u, buf, sizeof(buf));
    buf[3] = cmd::VERSION_DATA;
    GlobalView got{};
    TEST_ASSERT_FALSE(parse_global(buf, n, 0x01u, got));
}

void test_global_refuses_a_short_buffer(void) {
    uint8_t small[8] = {};
    TEST_ASSERT_EQUAL_UINT16(0u, build_global(sample_global(), 0x01u, small, sizeof(small)));
}

// The advertised maximum really does hold the largest frame a product can build.
void test_global_frame_max_holds_every_record(void) {
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    TEST_ASSERT_TRUE(build_global(sample_global(), 0x01u, buf, sizeof(buf)) > 0);

    // And the bound is summed from the table rather than restated, so a record added there
    // is budgeted for. Every row costs its tag, its length byte and its longest payload.
    uint16_t expect = 5u;
    for (const GlobalRecord& r : GLOBAL_RECORDS) expect = (uint16_t)(expect + 2u + r.max_len);
    TEST_ASSERT_EQUAL_UINT16(expect, GLOBAL_FRAME_MAX);
}

// The table covers the tag vocabulary, which is what stops a tag being named and then
// forgotten by the encoder, the decoder and the bound at once -- KNOB_MODE's first commits
// being exactly that.
void test_every_global_tag_has_a_record(void) {
    TEST_ASSERT_EQUAL_UINT8(global_tag::CHANNEL, GLOBAL_RECORDS[0].tag);
    TEST_ASSERT_EQUAL_UINT8(global_tag::LAST, GLOBAL_RECORDS[GLOBAL_RECORD_COUNT - 1u].tag);
    for (uint16_t i = 1u; i < GLOBAL_RECORD_COUNT; ++i)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(GLOBAL_RECORDS[i - 1u].tag + 1u), GLOBAL_RECORDS[i].tag);

    // Every row is reachable through the length bound the decoder consults.
    for (const GlobalRecord& r : GLOBAL_RECORDS)
        TEST_ASSERT_EQUAL_UINT8(r.min_len, global_min_len(r.tag));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, global_min_len(0x7Fu),
                                    "an unknown tag was given a length bound");
}

// The knob mode a host can name is one the firmware can send and read back.
void test_global_carries_the_knob_mode(void) {
    GlobalView g{};
    g.has_knob_mode = true;
    g.knob_mode     = 1u;                       // jump
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(g, 0x01u, buf, sizeof(buf));
    TEST_ASSERT_TRUE(all_data_bytes_7bit(buf, n));

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_TRUE_MESSAGE(got.has_knob_mode, "the knob mode was written but not read back");
    TEST_ASSERT_EQUAL_UINT8(1u, got.knob_mode);
}

// A product that has no knob mode to report does not send the record, and a host reading
// the frame is told nothing rather than told pickup.
void test_a_product_without_a_knob_mode_sends_no_record(void) {
    GlobalView g{};
    g.has_channel = true;
    uint8_t buf[GLOBAL_FRAME_MAX] = {};
    const uint16_t n = build_global(g, 0x01u, buf, sizeof(buf));

    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(buf, n, 0x01u, got));
    TEST_ASSERT_FALSE(got.has_knob_mode);
}

// An empty record names a field it cannot fill, and is left absent rather than read as a
// zero the pedal never sent. The bound is the table's, so this holds for every record.
void test_an_empty_knob_mode_record_is_left_absent(void) {
    const uint8_t frame[] = { 0xF0u, MANUFACTURER_ID, 0x01u, cmd::GLOBAL_DATA,
                              global_tag::KNOB_MODE, 0u,      // a record with no payload
                              0xF7u };
    GlobalView got{};
    TEST_ASSERT_TRUE(parse_global(frame, (uint16_t)sizeof(frame), 0x01u, got));
    TEST_ASSERT_FALSE_MESSAGE(got.has_knob_mode, "an empty record was half-believed");
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
    RUN_TEST(test_the_preset_frame_is_a_fixed_point);
    RUN_TEST(test_the_builder_writes_exactly_the_count_the_head_gives);
    RUN_TEST(test_the_builder_refuses_a_part_it_was_not_given);
    RUN_TEST(test_the_builder_refuses_a_command_the_parser_would_not_take);
    RUN_TEST(test_the_builder_refuses_a_short_buffer);
    RUN_TEST(test_a_built_preset_fits_the_advertised_maximum);
    RUN_TEST(test_the_slot_pair_round_trips);
    RUN_TEST(test_uid_matches_its_own);
    RUN_TEST(test_uid_rejects_another_unit);
    RUN_TEST(test_uid_rejects_a_high_byte_difference);
    RUN_TEST(test_uid_rejects_an_unaddressed_command);
    RUN_TEST(test_uid_rejects_a_truncated_payload);
    RUN_TEST(test_global_round_trips);
    RUN_TEST(test_global_routing_block_round_trips_every_field);
    RUN_TEST(test_global_carries_the_follow_sentinel);
    RUN_TEST(test_global_omits_the_fields_a_product_does_not_have);
    RUN_TEST(test_global_ext_input_without_the_hold_tail);
    RUN_TEST(test_global_with_no_records_is_valid);
    RUN_TEST(test_global_skips_a_tag_it_does_not_know);
    RUN_TEST(test_global_leaves_a_short_record_absent);
    RUN_TEST(test_global_skips_a_retired_record);
    RUN_TEST(test_global_rejects_another_product);
    RUN_TEST(test_global_rejects_an_unrelated_command);
    RUN_TEST(test_global_refuses_a_short_buffer);
    RUN_TEST(test_global_frame_max_holds_every_record);
    RUN_TEST(test_every_global_tag_has_a_record);
    RUN_TEST(test_the_routing_record_sits_where_the_wire_says);
    RUN_TEST(test_global_carries_the_knob_mode);
    RUN_TEST(test_a_product_without_a_knob_mode_sends_no_record);
    RUN_TEST(test_an_empty_knob_mode_record_is_left_absent);
    return UNITY_END();
}
