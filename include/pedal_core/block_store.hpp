#pragma once
#include <cstdint>
#include "crc16.hpp"

// The family's stored-block convention: every EEPROM block — preset record,
// settings, calibration, last-slot ring record, layout header — ends with a
// CRC-16/CCITT-FALSE over all preceding bytes, stored little-endian in the
// final two. The seal covers reserved 0xFF bytes too, so a later field can
// claim a reserved byte (0xFF as its uninitialised sentinel) and blocks
// written before that field existed still validate.
//
// Field layouts, magics and format versions are each product's own; this
// header owns only the seal.
namespace pedal_core::blocks {

inline void seal(uint8_t* buf, uint16_t size)
{
    const uint16_t crc = crc16_ccitt(buf, (uint16_t)(size - 2u));
    buf[size - 2u] = (uint8_t)(crc & 0xFFu);
    buf[size - 1u] = (uint8_t)(crc >> 8);
}

inline bool crc_ok(const uint8_t* buf, uint16_t size)
{
    const uint16_t stored = (uint16_t)(buf[size - 2u] | ((uint16_t)buf[size - 1u] << 8));
    return stored == crc16_ccitt(buf, (uint16_t)(size - 2u));
}

// --- Sealed-block I/O over eeprom:: ----------------------------------------
// load_block reads and CRC-checks; what the bytes MEAN — magics, versions,
// field offsets, sentinels — is the caller's to decode, because those differ
// per product. save_block seals in place (the tail two bytes are written) and
// stores. A blank page fails the CRC like any other unwritten data, so one
// check covers never-written, torn and bit-rotted blocks alike.
bool load_block(uint16_t addr, uint16_t size, uint8_t* buf);
bool save_block(uint16_t addr, uint16_t size, uint8_t* buf);

// --- The layout header ------------------------------------------------------
// One sealed block naming the whole map: a 32-bit magic (LE at [0]) and a
// 16-bit version (LE at [4]). A product stamps it LAST when rebuilding its
// store, so an interrupted rebuild reads as invalid and is retried on the
// next boot.
bool header_valid(uint16_t addr, uint16_t size, uint32_t magic, uint16_t version);
bool write_header(uint16_t addr, uint16_t size, uint32_t magic, uint16_t version);

// --- Fixed-size record slots ------------------------------------------------
bool read_record (uint16_t base, uint16_t index, uint16_t recsz, uint8_t* buf);
bool write_record(uint16_t base, uint16_t index, uint16_t recsz, const uint8_t* buf);

// --- The wear-leveled last-slot ring ---------------------------------------
// Records the active preset slot across power cycles without wearing one
// page out: each write lands on the next of `count` records, and boot scans
// for the newest by a sequence number compared modular-16 (so the counter
// wrapping past 0xFFFF keeps ordering). The record is the family shape —
// slot u16 LE at [0], sequence u16 LE at [2], a magic byte at [4] that
// rejects the erased and zeroed degenerate patterns outright, a reserved
// 0xFF byte, and the seal.
class SlotRing {
public:
    constexpr SlotRing(uint16_t addr, uint16_t count, uint16_t recsz,
                       uint8_t magic, uint16_t slot_bound)
        : m_addr(addr), m_count(count), m_recsz(recsz),
          m_magic(magic), m_slot_bound(slot_bound) {}

    // Scan for the newest valid record and prime the write cursor after it.
    // Returns the slot it names, or -1 with the cursor at the ring's start
    // when no record is valid (a blank ring, or all torn/rotted).
    int32_t restore();

    // Persist `slot` if it differs from the last record written; a failed
    // EEPROM write leaves the cursor unmoved, so the next call retries the
    // same record.
    void remember(uint16_t slot);

private:
    uint16_t m_addr, m_count, m_recsz;
    uint8_t  m_magic;
    uint16_t m_slot_bound;
    uint16_t m_seq_last       = 0;        // sequence of the last record written
    uint16_t m_write_idx      = 0;        // ring index the NEXT record lands on
    uint16_t m_last_persisted = 0xFFFFu;  // slot of the last record written
};

}  // namespace pedal_core::blocks
