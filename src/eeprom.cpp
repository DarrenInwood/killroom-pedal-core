#include <pedal_core/eeprom.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_config.hpp"
#include <cstring>

// 25xx-series SPI EEPROM, 16-bit address — 25LC256 (32 KB, 64-byte pages) and
// 25LC512 (64 KB, 128-byte pages) are both in range; the product states which
// through EEPROM_PAGE_BYTES. Parts above 64 KB need a third address byte and are
// not supported here. The chip select is framed around each command through
// hal::eeprom_cs (other devices on the shared bus assert their own CS).
//
// Instruction set: WREN 0x06, READ 0x03, WRITE 0x02, RDSR 0x05 (WIP = bit 0).
// A page write must be preceded by WREN and cannot cross a page boundary; after a
// write the part is busy (~5 ms) until the status WIP bit clears.

static constexpr uint8_t  CMD_WREN = 0x06u;
static constexpr uint8_t  CMD_READ = 0x03u;
static constexpr uint8_t  CMD_WRITE = 0x02u;
static constexpr uint8_t  CMD_RDSR = 0x05u;
static constexpr uint8_t  SR_WIP   = 0x01u;
static constexpr uint16_t PAGE_SIZE = EEPROM_PAGE_BYTES;

static_assert(PAGE_SIZE != 0u && (PAGE_SIZE & (PAGE_SIZE - 1u)) == 0u,
              "EEPROM_PAGE_BYTES must be a power of two — write chunking masks with it");

static inline void cs_low()  { pedal_core::hal::eeprom_cs(true); }
static inline void cs_high() { pedal_core::hal::eeprom_cs(false); }

// Set by the boot probe (init); false until the part proves it can store and read back a
// byte. It selects which backing store the public read()/write() use: the SPI part when
// true, the RAM mirror below when false.
static bool s_healthy = false;

// Volatile mirror of the stored map, used when no working EEPROM is fitted. It stands in
// for the part at the same addresses, so the settings and calibration blocks, the
// last-slot ring and the preset being worked on all keep functioning through their normal
// code paths — the app never learns where its bytes live. init() fills it with 0xFF so it
// presents as a fresh, blank device: storage_init() then finds no valid header, and every
// save lands here and survives until power-down.
//
// It does NOT mirror the whole map, because on a product whose store is larger than its
// RAM it cannot. Three regions instead:
//
//   head   [0, HEAD_END)              the layout header
//   tail   [TAIL_BASE, STORE_SIZE)    settings, calibration, the current-slot ring
//   slot   one window into the bulk region between them, adopted by whichever record
//          was written last
//
// So a pedal with a dead store still boots, still keeps its settings and calibration for
// the session, and still edits and saves the preset it is on; other slots read blank and
// the loader turns that into a synthetic default, exactly as it does for a blank part.
//
// A product whose store does fit in RAM sets HEAD_END and TAIL_BASE to 0: the tail then
// spans the whole map, every address lands in it, the window is never consulted, and the
// behaviour is byte-for-byte a full mirror. One code path, no product branch.
static constexpr uint32_t NO_SLOT = 0xFFFFFFFFu;

static uint8_t  s_head[EEPROM_MIRROR_HEAD_END   > 0u ? EEPROM_MIRROR_HEAD_END   : 1u];
static uint8_t  s_tail[EEPROM_STORE_SIZE - EEPROM_MIRROR_TAIL_BASE];
static uint8_t  s_slot[EEPROM_MIRROR_SLOT_BYTES > 0u ? EEPROM_MIRROR_SLOT_BYTES : 1u];
static uint32_t s_slot_addr = NO_SLOT;
static uint16_t s_slot_len  = 0u;

// Where an address lands in the tail mirror, or nullptr if it is below the span.
//
// The span is chosen at compile time because a product mirroring its whole store sets the
// base to zero, and there `addr >= base` is trivially true — an always-true bounds check
// worth removing rather than silencing. Specialising means the comparison is never compiled
// for a product that cannot use it.
//
// `if constexpr` does not serve here: GCC 7, which the products build with, diagnoses the
// discarded branch too. A class template rather than a function one, because the half a
// product does not use is then never instantiated — an unused function specialisation is a
// warning of its own. The preprocessor does not serve either: the base is a constexpr
// rather than a macro, so `#if` reads it as 0 and takes the whole-store branch for every
// product.
namespace {

template <uint16_t Base>
struct TailSpan {
    static uint8_t* of(uint16_t addr)
    {
        return (addr >= Base) ? &s_tail[addr - Base] : nullptr;
    }
};

template <>
struct TailSpan<0u> {
    static uint8_t* of(uint16_t addr) { return &s_tail[addr]; }   // the whole store is the tail
};

}  // namespace

// Where [addr, addr+len) lives in the mirror, or nullptr if nothing holds it. `adopt`
// re-points the bulk window at this span — a write does, a read does not.
static uint8_t* mirror_span(uint16_t addr, uint16_t len, bool adopt)
{
    const uint32_t end = (uint32_t)addr + len;
    if (end > EEPROM_STORE_SIZE) return nullptr;

    if (EEPROM_MIRROR_HEAD_END > 0u && end <= EEPROM_MIRROR_HEAD_END)
        return &s_head[addr];
    if (uint8_t* tail = TailSpan<EEPROM_MIRROR_TAIL_BASE>::of(addr)) return tail;

    if (EEPROM_MIRROR_SLOT_BYTES == 0u || len > EEPROM_MIRROR_SLOT_BYTES) return nullptr;
    if (adopt) {
        s_slot_addr = addr;
        s_slot_len  = len;
        return s_slot;
    }
    if (s_slot_addr != NO_SLOT && addr >= s_slot_addr
            && end <= (uint32_t)s_slot_addr + s_slot_len)
        return &s_slot[addr - s_slot_addr];
    return nullptr;
}

// Outside the map is a caller bug and fails, on the mirror as on the part. Inside it but
// unmirrored is a different thing and is not an error — see ram_read.
static bool in_map(uint16_t addr, uint16_t len)
{
    return (uint32_t)addr + len <= (uint32_t)EEPROM_STORE_SIZE;
}

static bool ram_write(uint16_t addr, const uint8_t* data, uint16_t len)
{
    if (!in_map(addr, len)) return false;
    uint8_t* p = mirror_span(addr, len, true);
    if (p == nullptr) return false;      // in the map, but nothing can hold it
    memcpy(p, data, len);
    return true;
}

static bool ram_read(uint16_t addr, uint8_t* data, uint16_t len)
{
    if (!in_map(addr, len)) return false;
    const uint8_t* p = mirror_span(addr, len, false);
    // Nothing holds it, so answer the way a blank device would rather than failing: the
    // caller's CRC check then rejects it and substitutes a default, which is the same
    // path an unwritten slot takes on a healthy part.
    if (p == nullptr) { memset(data, 0xFFu, len); return true; }
    memcpy(data, p, len);
    return true;
}

// Poll the status register until the write-in-progress bit clears. A healthy page write clears
// WIP in ~5 ms; a faulty or absent part never does, so the loop is bounded (~200 ms at 6 MHz —
// 40x a real write, yet quick enough that a chip-less board doesn't stall boot for seconds in
// the health probe). Kick the watchdog each spin so even this bounded wait can't feed a reset.
static bool wait_write_done()
{
    for (uint32_t t = 0; t < 50'000u; ++t) {
        watchdog::kick();
        cs_low();
        spi::write(&CMD_RDSR, 1);
        uint8_t sr = 0xFFu;
        spi::transfer(nullptr, &sr, 1);
        cs_high();
        if (!(sr & SR_WIP)) return true;
    }
    return false;
}

// Ungated primitives. The public write()/read() wrap these with the health gate; the boot
// probe below must reach the bus before s_healthy is decided, so it calls these directly.
static bool write_raw(uint16_t addr, const uint8_t* data, uint16_t len);
static bool read_raw(uint16_t addr, uint8_t* data, uint16_t len);

// One-time health probe: program a known sentinel to the dedicated scratch byte and read it
// back. This is the only reliable way to tell a working-but-blank device (reads 0xFF) from a
// dead/absent one (MISO floats to all-0xFF or all-0x00, or the write never completes) —
// reads alone can't, since a blank cell and a stuck-high bus look identical.
static bool probe()
{
    const uint8_t sentinel = 0xA5u;
    uint8_t w = sentinel;
    if (!write_raw(EEPROM_PROBE_ADDR, &w, 1)) return false;
    uint8_t r = (uint8_t)~sentinel;   // seed with the complement so a no-op read can't pass
    read_raw(EEPROM_PROBE_ADDR, &r, 1);
    return r == sentinel;
}

void eeprom::init()
{
    pedal_core::hal::eeprom_pins_init();   // CS configured and deselected; the
                                           // SPI bus itself is the product's to bring up
    s_healthy = probe();
    if (!s_healthy) {
        // Present the mirror as a fresh, blank device.
        memset(s_head, 0xFFu, sizeof(s_head));
        memset(s_tail, 0xFFu, sizeof(s_tail));
        memset(s_slot, 0xFFu, sizeof(s_slot));
        s_slot_addr = NO_SLOT;
        s_slot_len  = 0u;
    }
}

bool eeprom::healthy() { return s_healthy; }

bool eeprom::write(uint16_t addr, const uint8_t* data, uint16_t len)
{
    // Never write to a part that failed the probe: each page write would end in the
    // wait_write_done() timeout above, stalling the UI on every save (and, on the
    // factory-bank fill, the whole boot). The RAM mirror takes the write instead — instant,
    // and readable back for the rest of the session.
    if (!s_healthy) return ram_write(addr, data, len);
    return write_raw(addr, data, len);
}

bool eeprom::read(uint16_t addr, uint8_t* data, uint16_t len)
{
    // A faulty part returns nothing meaningful (MISO floats to all-0x00 or all-0xFF), so
    // read the mirror rather than the bus — it holds whatever this session has stored.
    if (!s_healthy) return ram_read(addr, data, len);
    return read_raw(addr, data, len);
}

static bool write_raw(uint16_t addr, const uint8_t* data, uint16_t len)
{
    while (len > 0) {
        // Never cross a page boundary in one write.
        const uint16_t page_offset = (uint16_t)(addr & (PAGE_SIZE - 1u));
        const uint16_t chunk = (uint16_t)((PAGE_SIZE - page_offset) < len
                                          ? (PAGE_SIZE - page_offset)
                                          : len);

        cs_low();
        spi::write(&CMD_WREN, 1);
        cs_high();

        const uint8_t hdr[3] = { CMD_WRITE, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFFu) };
        cs_low();
        spi::write(hdr, sizeof(hdr));
        spi::write(data, chunk);
        cs_high();

        if (!wait_write_done()) return false;

        addr += chunk;
        data += chunk;
        len  -= chunk;
    }
    return true;
}

static bool read_raw(uint16_t addr, uint8_t* data, uint16_t len)
{
    const uint8_t hdr[3] = { CMD_READ, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFFu) };
    cs_low();
    spi::write(hdr, sizeof(hdr));
    spi::transfer(nullptr, data, len);
    cs_high();
    return true;
}
