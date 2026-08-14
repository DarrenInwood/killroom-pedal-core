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

// Write "Received NN%" into buf (needs >= 14 bytes), null-terminated. Avoids snprintf
// so the bootloader stays small. pct is clamped to 100.
inline void format_received(char* buf, uint8_t pct)
{
    if (pct > 100u) pct = 100u;
    const char* p = "Received ";
    uint8_t i = 0;
    while (*p) buf[i++] = *p++;
    if (pct >= 100u) { buf[i++] = '1'; buf[i++] = '0'; buf[i++] = '0'; }
    else { if (pct >= 10u) buf[i++] = (char)('0' + pct / 10u); buf[i++] = (char)('0' + pct % 10u); }
    buf[i++] = '%';
    buf[i]   = 0;
}

}  // namespace dfu
