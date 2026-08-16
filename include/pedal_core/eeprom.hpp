#pragma once
#include <cstdint>

// SPI EEPROM driver (25xx256 family, 32 KB, 64-byte page) on the shared SPI bus.
// 16-bit byte address, page-bounded writes, blocking. Hardware is reached only
// through <pedal_core/hal.hpp>; the chip select is the product's business.
namespace eeprom {
    // init() runs a one-time write-readback probe against a dedicated scratch byte to
    // decide whether the part is present and responding (see healthy()). Call once at
    // startup, after the product has brought up its SPI bus.
    void init();

    // True when the boot probe confirmed a working EEPROM, i.e. storage is persistent.
    //
    // When false the part is absent/faulty and read()/write() are served from a volatile
    // RAM mirror instead of the bus — never from the part itself, so there are no
    // multi-second stalls. The mirror comes up blank (all 0xFF, like a fresh device), so
    // settings and the preset being worked on load and save normally for the session and
    // are lost at power-down. Callers use this only to tell the player that storage is not
    // persistent — they do not need to gate reads or writes on it.
    //
    // How much of the map the mirror holds is the product's choice (EEPROM_MIRROR_* in its
    // pedal_core_config.hpp): the whole thing where it fits in RAM, or the header, the
    // system blocks and one record window where it does not. Anything unmirrored reads
    // blank, which the caller's CRC check turns into a default — the same path an unwritten
    // slot takes on a healthy part.
    bool healthy();

    // Both operate on the stored map [0, EEPROM_STORE_SIZE) — plus, on a healthy part, the
    // probe byte. On the mirror, a range outside the map fails; a range inside it but
    // unmirrored fails to write and reads blank.
    bool write(uint16_t addr, const uint8_t* data, uint16_t len);
    bool read(uint16_t addr, uint8_t* data, uint16_t len);
}
