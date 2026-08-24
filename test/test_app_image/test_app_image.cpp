// Host-native unit tests for the DFU bootloader's boot-time image check
// (bootloader/src/app_image.hpp).
//
// The bootloader jumps to the application only after app_image::validate confirms
// the flashed image is complete and intact — that is what turns a firmware update
// aborted mid-flash into a "please re-run the updater" screen instead of a jump
// into broken code that crash-loops. The magic/size/bounds/trailer-placement logic
// is locked down here (the CRC math itself is covered by test_sysex_codec).

#include <unity.h>
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
    return UNITY_END();
}
