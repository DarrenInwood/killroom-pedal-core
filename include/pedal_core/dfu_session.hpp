#pragma once
#include <cstdint>
#include "sysex_codec.hpp"
#include "dfu_protocol.hpp"

// The largest chunk the session will accept, in binary bytes. See CHUNK_MAX.
#ifndef PEDAL_CORE_DFU_CHUNK_MAX
#define PEDAL_CORE_DFU_CHUNK_MAX 256u
#endif

// The DFU write session: everything between "a SysEx frame arrived" and "the
// bytes are in flash", with the flash itself behind a two-function seam.
//
// The point of the seam is testability. A bootloader's decode-and-bounds logic
// is the code that bricks a pedal when it is wrong, and it is exactly the code
// that a bootloader `main.cpp` — bound to CMSIS, TinyUSB and a real flash
// controller — cannot exercise on a host. Here it runs against a RAM-backed
// fake, so every rejection path has a test.
//
// The session owns no buffers larger than one chunk and allocates nothing.
namespace dfu {

// What the product's flash controller must provide. Both return false on any
// controller error; the session turns that into a Nack and stays in DFU, so a
// host can resend rather than leave a half-written image behind.
struct FlashOps {
    // A new upload is starting. The implementation forgets which pages it has
    // erased, so the first chunk touching a page erases it again. This is what
    // makes a retry work without a power cycle: a host that resends FW_BEGIN
    // after a failure must not end up programming over un-erased flash.
    // May be null if the product tracks nothing.
    void (*begin)(void* ctx);
    // Prepare [addr, addr+len) to be written. The implementation erases
    // whatever pages that range touches; the session calls this once per
    // chunk, and the implementation is expected to skip pages it has already
    // erased since the last begin().
    bool (*prepare)(uint32_t addr, uint32_t len, void* ctx);
    bool (*write)(uint32_t addr, const uint8_t* data, uint32_t len, void* ctx);
    // The region as the CPU reads it. On target this is simply the region base
    // cast to a pointer — flash is memory-mapped — but the session must not
    // assume that an address IS a pointer: on a 64-bit host, where these
    // functions are tested, a 32-bit address cannot hold one.
    const uint8_t* (*mapped)(void* ctx);
    void* ctx;
};

class Session {
public:
    // The writable application region, and how to write it.
    void begin(uint32_t region_base, uint32_t region_size, const FlashOps& ops)
    {
        m_base = region_base;
        m_size = region_size;
        m_ops  = ops;
        reset();
    }

    // Start (or restart) an upload. Resending FW_BEGIN is the documented way to
    // retry after a failure, so this must leave no state behind.
    void reset()
    {
        m_total = 0;
        m_written = 0;
        m_next_addr = m_base;
    }

    // Feed one complete SysEx frame, F0 ... F7, already checked for this
    // product's manufacturer and device bytes. `cmd` is frame[3]; `payload`
    // points at frame[4] and `plen` counts the bytes before the closing F7.
    //
    // Returns the status to report, and sets `reply_addr` to the address the
    // status frame carries: where the next byte is expected, which is what
    // lets a host resume mid-image instead of restarting.
    dfu_protocol::Status handle(uint8_t cmd, const uint8_t* payload, uint16_t plen,
                                uint32_t& reply_addr)
    {
        using dfu_protocol::Status;
        reply_addr = m_next_addr;

        switch (cmd) {
            case dfu_protocol::CMD_FW_BEGIN: {
                // 4 little-endian size bytes, 7-bit packed into 5.
                uint8_t sz[4] = {};
                if (sysex_codec::decode_7bit(payload, sz, plen, sizeof(sz)) != 4u)
                    return Status::Nack;
                const uint32_t total = (uint32_t)sz[0] | ((uint32_t)sz[1] << 8)
                                     | ((uint32_t)sz[2] << 16) | ((uint32_t)sz[3] << 24);
                if (total == 0u || total > m_size) return Status::Nack;
                reset();
                m_total = total;
                if (m_ops.begin) m_ops.begin(m_ops.ctx);   // forget which pages are erased
                reply_addr = m_next_addr;
                return Status::Ack;
            }

            case dfu_protocol::CMD_FW_CHUNK: {
                // Header: address as 4x7 bits, then the ENCODED payload length
                // as 2x7 bits. Seven-bit groups throughout, because every byte
                // between F0 and F7 must be <= 0x7F -- a raw 8-bit address byte
                // of 0x80 or above is a MIDI status byte and aborts the frame.
                // 28 bits reaches any flash address these parts have.
                if (plen < 6u) return Status::Nack;
                const uint32_t addr = ((uint32_t)payload[0] << 21) | ((uint32_t)payload[1] << 14)
                                    | ((uint32_t)payload[2] << 7)  |  (uint32_t)payload[3];
                const uint16_t enc_len = (uint16_t)(((uint16_t)payload[4] << 7) | payload[5]);
                const uint8_t* enc = &payload[6];

                // Every rejection below is a bricking bug if it is missing.
                if (enc_len == 0u) return Status::Nack;
                if ((uint32_t)enc_len + 6u > plen) return Status::Nack;   // must fit the frame
                const uint16_t len = sysex_codec::decoded_size(enc_len);
                if (len == 0u || len > CHUNK_MAX) return Status::Nack;
                if (addr < m_base) return Status::Nack;
                if (addr - m_base > m_size) return Status::Nack;
                if ((uint32_t)(addr - m_base) + len > m_size) return Status::Nack;
                if (addr & 3u) return Status::Nack;              // word-aligned writes only

                const uint16_t got = sysex_codec::decode_7bit(enc, m_chunk, enc_len, CHUNK_MAX);
                if (got < len) return Status::Nack;

                if (!m_ops.prepare(addr, len, m_ops.ctx)) return Status::Nack;
                if (!m_ops.write(addr, m_chunk, len, m_ops.ctx)) return Status::Nack;

                // High-water, not last-write: a host that resends an earlier
                // chunk must not rewind the progress it has already made, and
                // the end-of-image CRC is taken over what was actually written.
                const uint32_t end = addr + len;
                if (end > m_next_addr) m_next_addr = end;
                m_written  = m_next_addr - m_base;
                reply_addr = m_next_addr;
                return Status::Ack;
            }

            case dfu_protocol::CMD_FW_END: {
                uint8_t c[4] = {};
                if (sysex_codec::decode_7bit(payload, c, plen, sizeof(c)) != 4u)
                    return Status::Nack;
                const uint32_t want = (uint32_t)c[0] | ((uint32_t)c[1] << 8)
                                    | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
                // Over what was actually written, not what the host declared.
                // A declared size the upload never reached would CRC un-written
                // flash and fail with no clue where.
                const uint32_t len = m_written;
                if (len == 0u || len > m_size) return Status::Nack;
                const uint32_t got = sysex_codec::crc32_update(0, m_ops.mapped(m_ops.ctx), len);
                return (got == want) ? Status::Complete : Status::CrcFail;
            }

            default:
                return Status::Nack;
        }
    }

    uint32_t received() const { return m_written; }
    uint32_t total()    const { return m_total; }

    // The largest chunk a host may send. 256 binary bytes packs to 293 SysEx
    // bytes, comfortably inside a 512-byte receive buffer, and is what the
    // family's host updater sends. A product with a larger receive buffer and
    // an existing host that sends bigger chunks raises it by defining
    // PEDAL_CORE_DFU_CHUNK_MAX; the cost is that much RAM inside Session.
    static constexpr uint16_t CHUNK_MAX = PEDAL_CORE_DFU_CHUNK_MAX;

private:
    uint32_t m_base = 0, m_size = 0;
    uint32_t m_total = 0, m_written = 0, m_next_addr = 0;
    FlashOps m_ops = { nullptr, nullptr, nullptr, nullptr, nullptr };
    uint8_t  m_chunk[CHUNK_MAX] = {};
};

}  // namespace dfu
