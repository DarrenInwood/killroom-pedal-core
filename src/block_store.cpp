#include <pedal_core/block_store.hpp>
#include <pedal_core/eeprom.hpp>
#include <cstring>

namespace pedal_core::blocks {

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline void wr16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

// --- Sealed-block I/O -------------------------------------------------------

bool load_block(uint16_t addr, uint16_t size, uint8_t* buf)
{
    return eeprom::read(addr, buf, size) && crc_ok(buf, size);
}

bool save_block(uint16_t addr, uint16_t size, uint8_t* buf)
{
    seal(buf, size);
    return eeprom::write(addr, buf, size);
}

// --- The layout header ------------------------------------------------------

bool header_valid(uint16_t addr, uint16_t size, uint32_t magic, uint16_t version)
{
    uint8_t buf[64];
    if (size > sizeof(buf)) return false;
    if (!load_block(addr, size, buf)) return false;
    const uint32_t got = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                       | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return got == magic && rd16(&buf[4]) == version;
}

bool write_header(uint16_t addr, uint16_t size, uint32_t magic, uint16_t version)
{
    uint8_t buf[64];
    if (size > sizeof(buf)) return false;
    memset(buf, 0xFFu, size);   // reserved span reads as unclaimed
    buf[0] = (uint8_t)(magic & 0xFFu);
    buf[1] = (uint8_t)((magic >> 8) & 0xFFu);
    buf[2] = (uint8_t)((magic >> 16) & 0xFFu);
    buf[3] = (uint8_t)((magic >> 24) & 0xFFu);
    wr16(&buf[4], version);
    return save_block(addr, size, buf);
}

// --- Fixed-size record slots ------------------------------------------------

bool read_record(uint16_t base, uint16_t index, uint16_t recsz, uint8_t* buf)
{
    return eeprom::read((uint16_t)(base + index * recsz), buf, recsz);
}

bool write_record(uint16_t base, uint16_t index, uint16_t recsz, const uint8_t* buf)
{
    return eeprom::write((uint16_t)(base + index * recsz), buf, recsz);
}

// --- The wear-leveled last-slot ring ---------------------------------------

// The record's family shape (see the header): slot @0, seq @2, magic @4.
static constexpr uint8_t REC_SLOT  = 0u;
static constexpr uint8_t REC_SEQ   = 2u;
static constexpr uint8_t REC_MAGIC = 4u;

int32_t SlotRing::restore()
{
    bool     found     = false;
    uint16_t best_slot = 0;
    uint16_t best_seq  = 0;
    uint16_t best_idx  = 0;

    // One record at a time: the ring is scanned once at boot, and a whole-ring
    // buffer would put half a kilobyte on the stack for no gain.
    for (uint16_t i = 0; i < m_count; ++i) {
        uint8_t r[16];
        if (m_recsz > sizeof(r)) break;
        if (!read_record(m_addr, i, m_recsz, r)) continue;
        // The magic byte rejects the two degenerate patterns outright — an
        // erased (all-0xFF) and a zeroed (all-0x00) record — before the CRC
        // catches torn writes and bit rot.
        if (r[REC_MAGIC] != m_magic) continue;
        if (!crc_ok(r, m_recsz)) continue;
        const uint16_t slot = rd16(&r[REC_SLOT]);
        if (slot >= m_slot_bound) continue;
        const uint16_t seq = rd16(&r[REC_SEQ]);
        // Newest = greatest seq under modular-16 arithmetic (ring spread < 32768).
        if (!found || (uint16_t)(seq - best_seq) < 0x8000u) {
            found = true; best_slot = slot; best_seq = seq; best_idx = i;
        }
    }

    if (found) {
        m_seq_last       = best_seq;
        m_write_idx      = (uint16_t)((best_idx + 1u) % m_count);
        m_last_persisted = best_slot;
        return (int32_t)best_slot;
    }
    m_seq_last       = 0;
    m_write_idx      = 0;
    m_last_persisted = 0xFFFFu;
    return -1;
}

void SlotRing::remember(uint16_t slot)
{
    if (slot == m_last_persisted) return;  // only-if-changed: no redundant writes

    const uint16_t seq = (uint16_t)(m_seq_last + 1u);  // first real write -> seq >= 1
    uint8_t rec[16];
    if (m_recsz > sizeof(rec)) return;
    memset(rec, 0xFFu, m_recsz);   // reserved byte stays 0xFF
    wr16(&rec[REC_SLOT], slot);
    wr16(&rec[REC_SEQ],  seq);
    rec[REC_MAGIC] = m_magic;
    seal(rec, m_recsz);

    if (!write_record(m_addr, m_write_idx, m_recsz, rec)) return;

    m_seq_last       = seq;
    m_write_idx      = (uint16_t)((m_write_idx + 1u) % m_count);
    m_last_persisted = slot;
}

}  // namespace pedal_core::blocks
