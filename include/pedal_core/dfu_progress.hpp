#pragma once
// Pure helpers for the DFU upload progress display. Header-only and free of any
// hardware/CMSIS dependency so they compile on the host and are unit-tested by
// test/test_dfu_progress (the bootloader main.cpp itself is not host-compilable).
#include <cstdint>

namespace dfu {

// Upload percent (0..100) for `done` of `total` bytes. total == 0 means the size
// is unknown (no FW_BEGIN received) and yields 0. Saturates at 100 if done >= total.
inline uint8_t percent(uint32_t done, uint32_t total)
{
    if (total == 0u)   return 0u;
    if (done >= total) return 100u;
    // `done` is bounded by the app flash region (< 512 KB), so done*100 (< ~5.1e7)
    // fits in uint32 — keep it 32-bit so no 64-bit divide is pulled into the bootloader.
    return (uint8_t)(done * 100u / total);
}

// Write "<via> <text>" into buf, null-terminated, and return the length written.
//
// `via` names the transport an upload is arriving on, and may be null — the text then
// stands alone rather than carrying a leading space, which is what the screen shown before
// any host has claimed the session needs. Naming the wire earns its space on a 21-column
// line because the pedal listens on USB and MIDI DIN at once and only one of them drives
// any given upload: someone watching an updater that is getting no reply can then see on
// the pedal itself that the other wire holds the session, a state otherwise invisible from
// either end.
//
// No bounds check and no snprintf, so the bootloader stays small: every caller passes a
// fixed string into a buffer sized for the longest of them.
inline uint8_t format_status(char* buf, const char* via, const char* text)
{
    uint8_t i = 0;
    if (via) {
        while (*via) buf[i++] = *via++;
        buf[i++] = ' ';
    }
    while (*text) buf[i++] = *text++;
    buf[i] = 0;
    return i;
}

// Write "Received NN%" into buf (needs >= 14 bytes, plus strlen(via) + 1 when `via` is
// given), null-terminated. pct is clamped to 100.
inline void format_received(char* buf, uint8_t pct, const char* via = nullptr)
{
    if (pct > 100u) pct = 100u;
    uint8_t i = format_status(buf, via, "Received ");
    if (pct >= 100u) { buf[i++] = '1'; buf[i++] = '0'; buf[i++] = '0'; }
    else { if (pct >= 10u) buf[i++] = (char)('0' + pct / 10u); buf[i++] = (char)('0' + pct % 10u); }
    buf[i++] = '%';
    buf[i]   = 0;
}

}  // namespace dfu
