#pragma once
#include <cstdint>
#include <cstdio>

// The screenshot container: captured display frames on their way out of a host
// build and into the docs.
//
// A product documents its UI by compiling the real display driver and the real
// compositor for the host, driving them through the states it wants pictured,
// and calling display::capture() after each one. That leaves a page-format
// framebuffer in memory and nothing to look at. This writes those frames to a
// file that tools/oled_png.py turns into PNGs — so the pictures in the docs are
// the firmware's own output rather than a drawing of it, and no second
// implementation of the layout exists to drift.
//
// Each entry carries its caption twice: once as text, which names the PNG and
// its alt text, and once as a strip of pixels the producer has already rendered
// in the pedal's own small font. The pre-rendered strip is why the reader needs
// no font tables and no image library — it composites the contact sheet out of
// bits it was handed.
//
// Format, all integers little-endian:
//
//     'P' 'C' 'F' 'B' | u16 version | u16 count
//     entry:  u8 slug_len,  slug bytes        (no NUL)
//             u8 cap_len,   caption bytes     (no NUL)
//             u8 width, u8 pages,      frame[pages * width]
//             u8 cap_pages,            strip[cap_pages * width]
//
// A page is 8 pixel rows, bit 0 = the top row — the same layout display.cpp
// keeps its framebuffer in, so a frame is written and read without conversion.
// `count` is patched in on finish(), so a producer need not know how many
// screens it will emit before it emits them.
namespace frame_dump {

constexpr uint16_t VERSION      = 1u;
constexpr uint8_t  MAX_TEXT_LEN = 255u;   // slug and caption are length-prefixed by one byte

struct Writer {
    FILE*    f     = nullptr;
    uint16_t count = 0u;
    bool     ok    = false;
};

namespace detail {

    inline bool put_u8(FILE* f, uint8_t v) { return fputc((int)v, f) != EOF; }

    inline bool put_u16(FILE* f, uint16_t v)
    {
        return put_u8(f, (uint8_t)(v & 0xFFu)) && put_u8(f, (uint8_t)(v >> 8));
    }

    // Length of a NUL-terminated string, refusing anything the one-byte prefix
    // cannot express. <cstring> is avoided so the header stays a two-include file.
    inline bool measure(const char* s, uint8_t& len_out)
    {
        if (s == nullptr) return false;
        uint32_t n = 0u;
        while (s[n] != '\0') {
            if (++n > MAX_TEXT_LEN) return false;
        }
        len_out = (uint8_t)n;
        return true;
    }

    // A slug names a file the reader writes, so it is restricted here rather than
    // trusted there: ASCII letters, digits, dot, dash and underscore, never
    // leading with a dot. A producer that builds a slug from a display name gets
    // told at the point of the mistake instead of scattering odd filenames.
    inline bool slug_ok(const char* s, uint8_t len)
    {
        if (len == 0u || s[0] == '.') return false;
        for (uint8_t i = 0; i < len; ++i) {
            const char c = s[i];
            const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                 (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
            if (!allowed) return false;
        }
        return true;
    }

    inline bool put_text(FILE* f, const char* s, uint8_t len)
    {
        if (!put_u8(f, len)) return false;
        return len == 0u || fwrite(s, 1u, len, f) == len;
    }

    inline bool put_bits(FILE* f, const uint8_t* bits, uint32_t n)
    {
        return n == 0u || fwrite(bits, 1u, n, f) == n;
    }

}  // namespace detail

// Open `path` and write the header with a placeholder count. Every later call is
// a no-op once something has failed, so a producer can write its whole screen
// list and check once, at finish().
inline bool begin(Writer& w, const char* path)
{
    w.f     = fopen(path, "wb");
    w.count = 0u;
    w.ok    = w.f != nullptr;
    if (!w.ok) return false;

    w.ok = fputc('P', w.f) != EOF && fputc('C', w.f) != EOF &&
           fputc('F', w.f) != EOF && fputc('B', w.f) != EOF &&
           detail::put_u16(w.f, VERSION) && detail::put_u16(w.f, 0u);
    return w.ok;
}

// Append one screen. `frame` is `pages * width` bytes in page format; `strip` is
// the caption pre-rendered the same way, `strip_pages` tall. Pass strip_pages 0
// for an entry the contact sheet should label with nothing.
inline bool add(Writer& w, const char* slug, const char* caption,
                const uint8_t* frame, uint8_t width, uint8_t pages,
                const uint8_t* strip, uint8_t strip_pages)
{
    if (!w.ok) return false;

    uint8_t slug_len = 0u, cap_len = 0u;
    if (!detail::measure(slug, slug_len) || !detail::measure(caption, cap_len) ||
        !detail::slug_ok(slug, slug_len) || width == 0u || pages == 0u ||
        frame == nullptr || (strip_pages > 0u && strip == nullptr)) {
        w.ok = false;
        return false;
    }

    w.ok = detail::put_text(w.f, slug, slug_len) &&
           detail::put_text(w.f, caption, cap_len) &&
           detail::put_u8(w.f, width) && detail::put_u8(w.f, pages) &&
           detail::put_bits(w.f, frame, (uint32_t)pages * width) &&
           detail::put_u8(w.f, strip_pages) &&
           detail::put_bits(w.f, strip, (uint32_t)strip_pages * width);
    if (w.ok) ++w.count;
    return w.ok;
}

// Patch the entry count into the header and close. Returns false if anything
// along the way failed, so one check at the end covers the whole file.
inline bool finish(Writer& w)
{
    if (w.f == nullptr) return false;
    if (w.ok) {
        w.ok = fseek(w.f, 6L, SEEK_SET) == 0 && detail::put_u16(w.f, w.count);
    }
    const bool closed = fclose(w.f) == 0;
    w.f = nullptr;
    return w.ok && closed;
}

}  // namespace frame_dump
