#pragma once
#include <cstdint>

// Test-only controls for the in-RAM EEPROM fake (eeprom_fake.cpp), which
// implements the real eeprom:: API over a 32 KB array so storage machinery
// links and runs unmodified.
namespace eeprom_test {
    // Clear the whole store to 0xFF (an erased/blank device) and zero the
    // write counters. Call between tests.
    void reset();

    // Direct access to the backing store for raw byte-layout assertions.
    uint8_t peek(uint16_t addr);
    void    poke(uint16_t addr, uint8_t value);

    // Number of times a given byte address has been programmed via write()
    // since the last reset(). Lets wear-leveling tests prove no single cell
    // is hammered.
    uint32_t write_count(uint16_t addr);
}
