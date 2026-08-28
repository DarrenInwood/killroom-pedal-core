// Host-native unit tests for the DFU bootloader's boot-time image check
// (bootloader/src/app_image.hpp).
//
// The bootloader jumps to the application only after app_image::validate confirms
// the flashed image is complete and intact — that is what turns a firmware update
// aborted mid-flash into a "please re-run the updater" screen instead of a jump
// into broken code that crash-loops. The magic/size/bounds/trailer-placement logic
// is locked down here (the CRC math itself is covered by test_sysex_codec).

#include <unity.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <vector>
#include <pedal_core/app_image.hpp>

void setUp(void) {}
void tearDown(void) {}

// A plausible app partition size. Only the bound matters here, not any product's map.
static constexpr uint32_t REGION = 496u * 1024u;

static void put32(std::vector<uint8_t>& v, uint32_t off, uint32_t val) {
    v[off + 0] = (uint8_t)(val & 0xFF);
    v[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    v[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    v[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

// Build a well-formed image of `total` bytes: descriptor at HEADER_OFFSET and a
// correct CRC32 trailer over [0, total-4), exactly as tools/appimage_crc.py emits.
// The ident/version words default to the erased-flash value, so every validate() test
// below goes on proving that integrity does not depend on identity — a product that has
// not claimed those words must keep validating exactly as it did.
static std::vector<uint8_t> make_valid_image(uint32_t total,
                                             uint32_t ident = 0xFFFFFFFFu,
                                             uint32_t version = 0xFFFFFFFFu) {
    std::vector<uint8_t> img(total, 0x5A);
    put32(img, app_image::HEADER_OFFSET + 0,  app_image::MAGIC);
    put32(img, app_image::HEADER_OFFSET + 4,  total);
    put32(img, app_image::HEADER_OFFSET + 8,  ident);
    put32(img, app_image::HEADER_OFFSET + 12, version);
    const uint32_t body_len = total - 4u;
    put32(img, body_len, sysex_codec::crc32_update(0, img.data(), body_len));
    return img;
}

void test_valid_image_accepts(void) {
    auto img = make_valid_image(0x800);
    TEST_ASSERT_TRUE(app_image::validate(img.data(), REGION));
}

void test_minimum_size_image_accepts(void) {
    // The smallest image the check admits: descriptor + trailer, nothing else.
    const uint32_t total = app_image::HEADER_OFFSET + app_image::HEADER_SIZE + 4u;  // 0x214
    auto img = make_valid_image(total);
    TEST_ASSERT_TRUE(app_image::validate(img.data(), REGION));
}

void test_bad_magic_rejects(void) {
    auto img = make_valid_image(0x800);
    img[app_image::HEADER_OFFSET] ^= 0xFF;  // corrupt the marker
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

void test_body_corruption_rejects(void) {
    // A flipped byte anywhere in the body (here inside the code region) must fail
    // the CRC — this is the aborted/partial-flash case.
    auto img = make_valid_image(0x800);
    img[0x123] ^= 0x01;
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

void test_trailer_corruption_rejects(void) {
    auto img = make_valid_image(0x800);
    img[0x800 - 1] ^= 0x80;  // flip a bit in the stored CRC
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

void test_zero_size_rejects(void) {
    // An un-patched image (blank chip, or the ELF flashed without the post-build
    // step) reads size == 0 and must fail safe into DFU rather than be trusted.
    auto img = make_valid_image(0x800);
    put32(img, app_image::HEADER_OFFSET + 4, 0u);
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

void test_size_below_minimum_rejects(void) {
    auto img = make_valid_image(0x800);
    put32(img, app_image::HEADER_OFFSET + 4, app_image::HEADER_OFFSET);  // < min
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

void test_size_exceeds_region_rejects(void) {
    // A plausible-looking descriptor whose size overruns the app partition.
    auto img = make_valid_image(0x800);
    TEST_ASSERT_FALSE(app_image::validate(img.data(), /*region_bytes=*/0x400));
}

void test_unaligned_size_rejects(void) {
    auto img = make_valid_image(0x800);
    put32(img, app_image::HEADER_OFFSET + 4, 0x801u);  // not a multiple of 4
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}

// --- Identity ---------------------------------------------------------------------

void test_ident_and_version_round_trip(void) {
    auto img = make_valid_image(0x800,
                                app_image::make_ident(0x7D, 0x03),
                                app_image::make_version(2, 5, 9));
    TEST_ASSERT_EQUAL_UINT8(app_image::IDENT_FORMAT, app_image::ident_format(img.data()));
    TEST_ASSERT_EQUAL_UINT8(0x7D, app_image::ident_manufacturer(img.data()));
    TEST_ASSERT_EQUAL_UINT8(0x03, app_image::ident_device(img.data()));
    TEST_ASSERT_EQUAL_UINT8(2, app_image::version_major(img.data()));
    TEST_ASSERT_EQUAL_UINT8(5, app_image::version_minor(img.data()));
    TEST_ASSERT_EQUAL_UINT8(9, app_image::version_patch(img.data()));
}

void test_identity_accepts_its_own_product(void) {
    auto img = make_valid_image(0x800, app_image::make_ident(0x7D, 0x01));
    TEST_ASSERT_TRUE(app_image::identity_matches(img.data(), 0x7D, 0x01));
}

// The case the check exists for: a sibling pedal's image is sealed just as correctly and
// passes every integrity test, so identity is the only thing that can turn it away.
void test_identity_rejects_a_sibling_product(void) {
    auto img = make_valid_image(0x800, app_image::make_ident(0x7D, 0x02));
    TEST_ASSERT_TRUE(app_image::validate(img.data(), REGION));
    TEST_ASSERT_FALSE(app_image::identity_matches(img.data(), 0x7D, 0x01));
}

void test_identity_rejects_a_foreign_manufacturer(void) {
    auto img = make_valid_image(0x800, app_image::make_ident(0x41, 0x01));
    TEST_ASSERT_FALSE(app_image::identity_matches(img.data(), 0x7D, 0x01));
}

// An unclaimed descriptor reads as format 0xFF. It must be refused, which is why a
// bootloader enforcing identity has to ship on a product's very first unit.
void test_identity_rejects_an_unclaimed_descriptor(void) {
    auto img = make_valid_image(0x800);   // ident left at the erased-flash value
    TEST_ASSERT_TRUE(app_image::validate(img.data(), REGION));
    TEST_ASSERT_FALSE(app_image::identity_matches(img.data(), 0x7D, 0x01));
}

// The must-ignore high bytes are the only room a shipped bootloader will ever have to
// accept something new, so a reader has to mask them off rather than compare the word.
// If this test fails, that expansion room is gone for every unit already in the field.
void test_reserved_high_bytes_are_ignored(void) {
    auto img = make_valid_image(0x800,
                                app_image::make_ident(0x7D, 0x01) | 0xAB000000u,
                                app_image::make_version(1, 2, 3) | 0xCD000000u);
    TEST_ASSERT_TRUE(app_image::identity_matches(img.data(), 0x7D, 0x01));
    TEST_ASSERT_EQUAL_UINT8(1, app_image::version_major(img.data()));
    TEST_ASSERT_EQUAL_UINT8(2, app_image::version_minor(img.data()));
    TEST_ASSERT_EQUAL_UINT8(3, app_image::version_patch(img.data()));
}

// Identity is a separate predicate from integrity: a corrupt image must fail validate()
// whatever its descriptor claims, and the two must not be folded together.
void test_identity_does_not_imply_integrity(void) {
    auto img = make_valid_image(0x800, app_image::make_ident(0x7D, 0x01));
    img[0x400] ^= 0x01;   // corrupt the body, leave the descriptor intact
    TEST_ASSERT_TRUE(app_image::identity_matches(img.data(), 0x7D, 0x01));
    TEST_ASSERT_FALSE(app_image::validate(img.data(), REGION));
}


// ---------------------------------------------------------------------------
// The shared fixtures
//
// The descriptor contract has three implementations: validate() here, the sealer
// that writes the descriptor, and the release gate that re-checks it. Two of them
// are Python. Nothing used to hold the three to the same bytes, and a divergence
// does not crash -- it ships an image the bootloader refuses and leaves the pedal
// in DFU.
//
// So both sides walk one manifest. tools/test_app_image.py runs these same rows
// through the gate's check(); this runs them through validate(). A row where the
// two are meant to differ says so in its note, and both suites hold it to that.
// ---------------------------------------------------------------------------

namespace {

struct FixtureRow {
    std::string file;
    uint32_t    region_bytes;
    std::string cpp;       // "valid", "invalid", or "n-a"
};

std::string fixture_path(const char* name)
{
    return std::string(APP_IMAGE_FIXTURE_DIR) + "/" + name;
}

// Read a whole fixture. Returns empty on failure, which the caller reports as a
// missing fixture rather than treating as an image.
std::vector<uint8_t> read_file(const std::string& path)
{
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

// The manifest, minus its comments and its header line. Tab-separated so both a
// C++ suite and a Python one can read it without a parser.
std::vector<FixtureRow> read_manifest()
{
    std::vector<FixtureRow> rows;
    const std::vector<uint8_t> raw = read_file(fixture_path("manifest.tsv"));
    if (raw.empty()) return rows;

    std::string text(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1u : nl + 1u;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::string field[3];
        size_t start = 0;
        bool short_row = false;
        for (uint8_t i = 0; i < 3u; ++i) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos && i < 2u) { short_row = true; break; }
            field[i] = line.substr(start, (tab == std::string::npos) ? std::string::npos : tab - start);
            start = tab + 1u;
        }
        if (short_row || field[0] == "file") continue;

        FixtureRow r;
        r.file         = field[0];
        r.region_bytes = (uint32_t)strtoul(field[1].c_str(), nullptr, 10);
        r.cpp          = field[2];
        rows.push_back(r);
    }
    return rows;
}

}  // namespace

// Every fixture earns the verdict the manifest records for this side. A row marked
// n-a names a case validate() cannot be asked -- a buffer too short to hold a
// descriptor, which it reads before it knows any size -- and is skipped here while
// the Python side still judges it.
void test_fixtures_earn_the_verdict_the_manifest_records(void) {
    const std::vector<FixtureRow> rows = read_manifest();
    TEST_ASSERT_TRUE_MESSAGE(!rows.empty(), "the manifest read as empty");

    uint8_t judged = 0;
    for (const FixtureRow& r : rows) {
        if (r.cpp == "n-a") continue;

        const std::vector<uint8_t> img = read_file(fixture_path(r.file.c_str()));
        TEST_ASSERT_TRUE_MESSAGE(!img.empty(), r.file.c_str());

        const bool got      = app_image::validate(img.data(), r.region_bytes);
        const bool expected = (r.cpp == "valid");
        TEST_ASSERT_EQUAL_MESSAGE(expected, got, r.file.c_str());
        ++judged;
    }

    // Guards against a manifest that parsed into nothing useful, or one whose rows
    // are all skipped -- either would make this test pass while checking nothing.
    TEST_ASSERT_GREATER_THAN_UINT8_MESSAGE(4, judged, "too few fixtures judged");
}

// The one row the two sides are meant to disagree on, pinned from this side so the
// disagreement stays deliberate. validate() judges a region of flash and reads only
// `size` bytes, so trailing bytes past the declared size are none of its business;
// the release gate judges a file and refuses a size that disagrees with the length
// on disk. Both are right about the question they are asked.
void test_trailing_bytes_are_a_difference_in_the_question(void) {
    const std::vector<uint8_t> img = read_file(fixture_path("trailing_bytes.bin"));
    TEST_ASSERT_TRUE(!img.empty());
    TEST_ASSERT_TRUE(app_image::validate(img.data(), 24576u));
    // And the declared size really is shorter than the file, or this proves nothing.
    const uint32_t declared = app_image::rd32_le(img.data() + app_image::HEADER_OFFSET + 4u);
    TEST_ASSERT_TRUE(declared < img.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_image_accepts);
    RUN_TEST(test_minimum_size_image_accepts);
    RUN_TEST(test_bad_magic_rejects);
    RUN_TEST(test_body_corruption_rejects);
    RUN_TEST(test_trailer_corruption_rejects);
    RUN_TEST(test_zero_size_rejects);
    RUN_TEST(test_size_below_minimum_rejects);
    RUN_TEST(test_size_exceeds_region_rejects);
    RUN_TEST(test_unaligned_size_rejects);
    RUN_TEST(test_ident_and_version_round_trip);
    RUN_TEST(test_identity_accepts_its_own_product);
    RUN_TEST(test_identity_rejects_a_sibling_product);
    RUN_TEST(test_identity_rejects_a_foreign_manufacturer);
    RUN_TEST(test_identity_rejects_an_unclaimed_descriptor);
    RUN_TEST(test_reserved_high_bytes_are_ignored);
    RUN_TEST(test_identity_does_not_imply_integrity);
    RUN_TEST(test_fixtures_earn_the_verdict_the_manifest_records);
    RUN_TEST(test_trailing_bytes_are_a_difference_in_the_question);
    return UNITY_END();
}
