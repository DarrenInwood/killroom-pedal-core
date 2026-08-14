#include <pedal_core/eeprom.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_config.hpp"
#include <cstring>

// 25xx256 SPI EEPROM. 32 KB, 64-byte pages, 16-bit address. The chip select is
// framed around each command through hal::eeprom_cs (other devices on the shared
// bus assert their own CS).
//
// Instruction set: WREN 0x06, READ 0x03, WRITE 0x02, RDSR 0x05 (WIP = bit 0).
// A page write must be preceded by WREN and cannot cross a 64-byte page boundary;
// after a write the part is busy (~5 ms) until the status WIP bit clears.

static constexpr uint8_t  CMD_WREN = 0x06u;
static constexpr uint8_t  CMD_READ = 0x03u;
static constexpr uint8_t  CMD_WRITE = 0x02u;
static constexpr uint8_t  CMD_RDSR = 0x05u;
static constexpr uint8_t  SR_WIP   = 0x01u;
static constexpr uint8_t  PAGE_SIZE = 64u;

static inline void cs_low()  { pedal_core::hal::eeprom_cs(true); }
static inline void cs_high() { pedal_core::hal::eeprom_cs(false); }

// Set by the boot probe (init); false until the part proves it can store and read back a
// byte. It selects which backing store the public read()/write() use: the SPI part when
// true, the RAM mirror below when false.
static bool s_healthy = false;

// Volatile mirror of the stored map, used when no working EEPROM is fitted. It stands in
// for the part byte-for-byte at the same addresses, so presets, the settings and
// calibration blocks and the last-slot ring all keep working through their normal code
// paths — the app never learns where its bytes live. init() fills it with 0xFF so it
// presents as a fresh, blank device: storage_init() then finds no valid header and writes
// the factory bank into it, and every save lands here and survives until power-down.
static uint8_t s_ram_store[EEPROM_STORE_SIZE];

static bool ram_write(uint16_t addr, const uint8_t* data, uint16_t len)
{
    if ((uint32_t)addr + len > sizeof(s_ram_store)) return false;
    memcpy(&s_ram_store[addr], data, len);
    return true;
}

static bool ram_read(uint16_t addr, uint8_t* data, uint16_t len)
{
    if ((uint32_t)addr + len > sizeof(s_ram_store)) return false;
    memcpy(data, &s_ram_store[addr], len);
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
    if (!s_healthy)
        memset(s_ram_store, 0xFFu, sizeof(s_ram_store));   // present the mirror as a blank device
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
        const uint8_t  page_offset = addr & (PAGE_SIZE - 1u);
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
