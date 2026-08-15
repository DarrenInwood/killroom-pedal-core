// In-RAM EEPROM fake: the real eeprom:: API over a 32 KB array, with per-byte
// write counters for wear assertions. Suites that need the REAL driver compile
// eeprom.cpp into their own TU (test_eeprom); this fake serves everyone else
// from the support library.
#include <pedal_core/eeprom.hpp>
#include "eeprom_fake.hpp"
#include <cstring>

namespace {
    constexpr uint32_t STORE = 32768u;
    uint8_t  g_mem[STORE];
    uint32_t g_writes[STORE];
}

namespace eeprom {
    void init() {}
    bool healthy() { return true; }
    bool write(uint16_t addr, const uint8_t* data, uint16_t len)
    {
        if ((uint32_t)addr + len > STORE) return false;
        for (uint16_t i = 0; i < len; ++i) {
            g_mem[addr + i] = data[i];
            ++g_writes[addr + i];
        }
        return true;
    }
    bool read(uint16_t addr, uint8_t* data, uint16_t len)
    {
        if ((uint32_t)addr + len > STORE) return false;
        memcpy(data, &g_mem[addr], len);
        return true;
    }
}

namespace eeprom_test {
    void reset()
    {
        memset(g_mem, 0xFFu, sizeof(g_mem));
        memset(g_writes, 0, sizeof(g_writes));
    }
    uint8_t  peek(uint16_t addr)               { return g_mem[addr]; }
    void     poke(uint16_t addr, uint8_t v)    { g_mem[addr] = v; }
    uint32_t write_count(uint16_t addr)        { return g_writes[addr]; }
}
