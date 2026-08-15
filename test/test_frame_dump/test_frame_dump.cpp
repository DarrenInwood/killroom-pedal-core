// Host-native unit tests for the screenshot container (pedal_core/frame_dump.hpp).
//
// The writer here and the reader in tools/oled_png.py are the two halves of one
// format, and only one of them is compiled. These tests pin the byte layout the
// Python side parses — header, length-prefixed text, page-format payloads, the
// entry count patched in at finish() — so a change to either half that the other
// did not follow fails here rather than in a docs build.
//
// The rejections matter for the same reason: a slug names a file the reader
// writes, and an over-long or path-shaped one must be refused at the point it is
// produced, not silently turned into an odd filename.

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <pedal_core/frame_dump.hpp>

static const char* const PATH = "test_frame_dump.bin";

void setUp(void) {}
void tearDown(void) { remove(PATH); }

// A recognisable 128x64 frame: byte n of page p is p*16 + (n & 0x0F).
static std::vector<uint8_t> make_frame(uint8_t width, uint8_t pages)
{
    std::vector<uint8_t> v((size_t)width * pages);
    for (uint8_t p = 0; p < pages; ++p)
        for (uint8_t x = 0; x < width; ++x)
            v[(size_t)p * width + x] = (uint8_t)(p * 16u + (x & 0x0Fu));
    return v;
}

static std::vector<uint8_t> read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    std::vector<uint8_t> out;
    int c;
    while ((c = fgetc(f)) != EOF) out.push_back((uint8_t)c);
    fclose(f);
    return out;
}

// --- A cursor over the written bytes, mirroring what the Python reader does. ---
struct Cursor {
    const std::vector<uint8_t>& d;
    size_t i = 0;
    uint8_t  u8()  { return d[i++]; }
    uint16_t u16() { const uint16_t v = (uint16_t)(d[i] | (d[i + 1] << 8)); i += 2; return v; }
    std::string text() { const uint8_t n = u8(); std::string s((const char*)&d[i], n); i += n; return s; }
};

static void write_two(uint8_t strip_pages)
{
    const auto frame = make_frame(128u, 8u);
    const auto strip = make_frame(128u, strip_pages ? strip_pages : 1u);

    frame_dump::Writer w;
    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_TRUE(frame_dump::add(w, "00_perf", "Clean Boost - Page 1/2",
                                     frame.data(), 128u, 8u, strip.data(), strip_pages));
    TEST_ASSERT_TRUE(frame_dump::add(w, "01_splash", "Splash",
                                     frame.data(), 128u, 8u, strip.data(), strip_pages));
    TEST_ASSERT_TRUE(frame_dump::finish(w));
}

void test_header_and_count(void)
{
    write_two(1u);
    const auto d = read_file(PATH);
    TEST_ASSERT_GREATER_THAN(8u, d.size());
    TEST_ASSERT_EQUAL_UINT8('P', d[0]);
    TEST_ASSERT_EQUAL_UINT8('C', d[1]);
    TEST_ASSERT_EQUAL_UINT8('F', d[2]);
    TEST_ASSERT_EQUAL_UINT8('B', d[3]);

    Cursor c{ d, 4 };
    TEST_ASSERT_EQUAL_UINT16(frame_dump::VERSION, c.u16());
    // The count is a placeholder until finish() seeks back and patches it.
    TEST_ASSERT_EQUAL_UINT16(2u, c.u16());
}

void test_entries_round_trip(void)
{
    write_two(1u);
    const auto d = read_file(PATH);
    const auto frame = make_frame(128u, 8u);
    const auto strip = make_frame(128u, 1u);

    Cursor c{ d, 8 };  // past magic + version + count
    static const char* const slugs[2]    = { "00_perf", "01_splash" };
    static const char* const captions[2] = { "Clean Boost - Page 1/2", "Splash" };

    for (int e = 0; e < 2; ++e) {
        TEST_ASSERT_EQUAL_STRING(slugs[e], c.text().c_str());
        TEST_ASSERT_EQUAL_STRING(captions[e], c.text().c_str());
        TEST_ASSERT_EQUAL_UINT8(128u, c.u8());
        TEST_ASSERT_EQUAL_UINT8(8u, c.u8());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(frame.data(), &d[c.i], (int)frame.size());
        c.i += frame.size();
        TEST_ASSERT_EQUAL_UINT8(1u, c.u8());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(strip.data(), &d[c.i], (int)strip.size());
        c.i += strip.size();
    }
    // Nothing after the last entry: the reader treats trailing bytes as an error.
    TEST_ASSERT_EQUAL_UINT32(d.size(), c.i);
}

void test_no_caption_strip_is_a_zero_length_payload(void)
{
    write_two(0u);
    const auto d = read_file(PATH);
    const auto frame = make_frame(128u, 8u);

    Cursor c{ d, 8 };
    (void)c.text();
    (void)c.text();
    c.i += 2u + frame.size();          // width, pages, frame
    TEST_ASSERT_EQUAL_UINT8(0u, c.u8());  // strip_pages, and no strip bytes follow
}

void test_empty_dump_is_well_formed(void)
{
    frame_dump::Writer w;
    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_TRUE(frame_dump::finish(w));

    const auto d = read_file(PATH);
    TEST_ASSERT_EQUAL_UINT32(8u, d.size());
    Cursor c{ d, 6 };
    TEST_ASSERT_EQUAL_UINT16(0u, c.u16());
}

void test_bad_slug_is_refused(void)
{
    const auto frame = make_frame(128u, 8u);
    static const char* const bad[] = {
        "",                 // unnamed
        ".hidden",          // leading dot
        "../escape",        // path traversal, and a slash
        "has space",
        "page1/2",          // a caption's separator is not a filename's
    };

    for (const char* slug : bad) {
        frame_dump::Writer w;
        TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
        TEST_ASSERT_FALSE(frame_dump::add(w, slug, "x", frame.data(), 128u, 8u, nullptr, 0u));
        TEST_ASSERT_FALSE(frame_dump::finish(w));   // one failure poisons the file
        remove(PATH);
    }
}

void test_over_long_text_is_refused(void)
{
    const auto frame = make_frame(128u, 8u);
    const std::string long_caption(frame_dump::MAX_TEXT_LEN + 1u, 'x');

    frame_dump::Writer w;
    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_FALSE(frame_dump::add(w, "ok", long_caption.c_str(),
                                      frame.data(), 128u, 8u, nullptr, 0u));
    TEST_ASSERT_FALSE(frame_dump::finish(w));
}

void test_missing_payload_is_refused(void)
{
    const auto frame = make_frame(128u, 8u);
    frame_dump::Writer w;

    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_FALSE(frame_dump::add(w, "ok", "x", nullptr, 128u, 8u, nullptr, 0u));
    TEST_ASSERT_FALSE(frame_dump::finish(w));
    remove(PATH);

    // A strip promised but not supplied is the same mistake one field over.
    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_FALSE(frame_dump::add(w, "ok", "x", frame.data(), 128u, 8u, nullptr, 1u));
    TEST_ASSERT_FALSE(frame_dump::finish(w));
    remove(PATH);

    // A zero dimension would make the reader's payload length zero and desynchronise it.
    TEST_ASSERT_TRUE(frame_dump::begin(w, PATH));
    TEST_ASSERT_FALSE(frame_dump::add(w, "ok", "x", frame.data(), 0u, 8u, nullptr, 0u));
    TEST_ASSERT_FALSE(frame_dump::finish(w));
}

void test_unopenable_path_fails_at_begin(void)
{
    frame_dump::Writer w;
    TEST_ASSERT_FALSE(frame_dump::begin(w, "no_such_dir/frames.bin"));
    TEST_ASSERT_FALSE(frame_dump::finish(w));   // and finish() does not crash on it
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_and_count);
    RUN_TEST(test_entries_round_trip);
    RUN_TEST(test_no_caption_strip_is_a_zero_length_payload);
    RUN_TEST(test_empty_dump_is_well_formed);
    RUN_TEST(test_bad_slug_is_refused);
    RUN_TEST(test_over_long_text_is_refused);
    RUN_TEST(test_missing_payload_is_refused);
    RUN_TEST(test_unopenable_path_fails_at_begin);
    return UNITY_END();
}
