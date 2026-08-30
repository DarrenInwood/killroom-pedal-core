#pragma once
#include <cstdint>

// SPI EEPROM driver for the 25xx family on a 16-bit byte address — 25LC256 (32 KB,
// 64-byte pages) through 25LC512 (64 KB, 128-byte pages) — on the shared SPI bus.
// Page-bounded writes at the size the product states in EEPROM_PAGE_BYTES, blocking.
// Parts above 64 KB need a third address byte and are out of range. Hardware is
// reached only through <pedal_core/hal.hpp>; the chip select is the product's business.
// The interface is wider than the four functions below: this driver reads six constants
// from the product's pedal_core_config.hpp, and none of them has a default here because
// every one is a fact about the part on the board rather than a preference.
//
//   EEPROM_PAGE_BYTES         a write may not cross a page boundary, and assuming a LARGER
//                             page than the part has makes it wrap and corrupt
//   EEPROM_STORE_SIZE         the addressable stored map
//   EEPROM_PROBE_ADDR         the scratch byte the boot health probe writes, outside it
//   EEPROM_MIRROR_HEAD_END    what the dead-part RAM mirror holds: the header, the system
//   EEPROM_MIRROR_TAIL_BASE   blocks, and a window big enough for one record of the bulk
//   EEPROM_MIRROR_SLOT_BYTES  region between them -- all three zero to mirror the map whole
//
// test/support/pedal_core_config.hpp is the reference and says what each one means.
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
    // are lost at power-down. Its extent is the product's to choose: a small store is
    // mirrored whole, and one too large for RAM keeps the header, the system blocks and a
    // one-record window, so other slots read blank and the loader substitutes a default —
    // the same path an unwritten slot takes on a healthy part. Callers use this only to
    // tell the player that storage is not persistent — they do not need to gate reads or
    // writes on it.
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
