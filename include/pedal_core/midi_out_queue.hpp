#pragma once
#include <cstdint>

// The MIDI Out jack's transmit queue: whole messages, and the rules deciding which survive
// when there are more than the wire can carry.
//
// MIDI In and MIDI Out share 31.25 kbaud, so a pedal that both forwards an inbound stream and
// speaks for itself has more to send than the jack can take, and USB In runs orders of
// magnitude faster still. Something has to be lost. This decides what, where messages are
// whole — a byte-level drop truncates a frame, and a receiver cannot tell a truncated preset
// dump from a good one.
//
// Messages rather than bytes, because coalescing needs identity and identity needs message
// boundaries: a byte ring cannot find the earlier value of CC 74 on channel 3 in order to
// replace it. The byte ring stays underneath, as staging for the wire.
//
// What leaves the jack is a controller's current value rather than every intermediate one.
// That is right for continuous state and wrong for everything else, so the exclusions below
// are most of what this module is.
//
// It holds canonical messages and owns no wire representation: running status is decided at
// drain, because coalescing and eviction rewrite the queue after a message enters it and what
// status byte is already on the wire is not knowable until transmission.
//
// See ADR 0004 in DarrenInwood/killroom-analog-multi-effect for the rejected alternatives.
namespace pedal_core {

// One MIDI message as it will be sent: a status byte and up to two data bytes. Long enough
// for every channel message and the System Common ones that reach the jack; SysEx streams
// under the frame lock instead and never enters the queue.
struct MidiMessage {
    uint8_t status  = 0u;
    uint8_t data[2] = {};
    uint8_t len     = 0u;   // 1..3, counting the status byte
};

class MidiOutQueue {
public:
    // A quarter of a second of backlog at the thousand-odd short messages a second the jack
    // drains, which is the latency this depth is spending rather than the RAM. Coalescing
    // bounds the continuous-controller working set whatever the depth, so the depth is spent
    // almost entirely on traffic that cannot coalesce — notes, program changes, NRPN
    // sequences, timecode. A reader raising it is buying backlog, not headroom.
    static constexpr uint16_t DEPTH = 256u;

    // Where a message came from. The pedal's own traffic is low-rate by construction, so it
    // cannot itself cause an overflow -- and a pedal unable to speak on its own jack fails
    // worse than one dropping a forwarded controller sweep.
    enum class Origin : uint8_t { Forwarded, Own };

    // Offer a message. Returns whether it will be sent.
    //
    // A message whose identity is already queued replaces the one there, keeping its place in
    // the order: the jack then sends the controller's current value rather than every step it
    // passed through, and a sweep costs one entry however long it runs.
    //
    // At a full queue something has to be lost. An ordinary forwarded message is refused --
    // the backlog already queued is worth more than the newest controller step. A message
    // that would leave a note hanging, and anything the pedal is saying for itself, is
    // admitted instead and pays for its place; see victim() for what it costs.
    bool push(const MidiMessage& m, Origin origin)
    {
        if (m.len == 0u) return false;

        if (coalescable(m)) {
            for (uint16_t i = 0u; i < m_count; ++i) {
                Entry& held = m_slot[index(i)];
                if (coalescable(held.msg) && same_identity(held.msg, m)) {
                    held.msg    = m;
                    held.origin = origin;   // it now carries what was most recently said
                    return true;
                }
            }
        }

        if (m_count >= DEPTH) {
            if (!protects_a_note(m) && origin != Origin::Own) return false;
            evict(victim(origin));
        }

        m_slot[index(m_count)] = Entry{ m, origin };
        ++m_count;
        return true;
    }

    // The next message to send, without taking it. A drain has to know how long a message
    // will be on the wire before it commits to sending it.
    bool peek(MidiMessage& out) const
    {
        if (m_count == 0u) return false;
        out = m_slot[m_head].msg;
        return true;
    }

    // The next message to send, in order. False when there is nothing.
    bool pop(MidiMessage& out)
    {
        if (m_count == 0u) return false;
        out    = m_slot[m_head].msg;
        m_head = (uint16_t)((m_head + 1u) % DEPTH);
        --m_count;
        return true;
    }

    // Whether losing this message would leave a note sounding with nothing to stop it. A
    // dropped note on is a missing note; a dropped note off is a stuck one, and the two are
    // not worth the same. Both spellings of note off count, because a keyboard chooses which
    // to send and protecting one would hang notes on half the controllers in the world.
    static bool protects_a_note(const MidiMessage& m)
    {
        const uint8_t type = (uint8_t)(m.status & 0xF0u);
        if (type == 0x80u) return true;
        if (type == 0x90u && m.len >= 3u && m.data[1] == 0u) return true;
        if (type == 0xB0u && (m.data[0] == 120u || m.data[0] == 123u)) return true;  // all sound/notes off
        return false;
    }

    uint16_t size() const { return m_count; }
    bool     empty() const { return m_count == 0u; }
    void     clear() { m_head = 0u; m_count = 0u; }

    // Whether a message is continuous state, so a later one of the same identity replaces it
    // rather than queueing behind it. Public because it is the rule this module exists for,
    // and a truth table over the controller numbers is the shape it reads in.
    //
    // Excluded, and so first-in first-out:
    //   CC 6, 38, 96-101  the RPN/NRPN transaction — a data-entry value against a stale
    //                     parameter select is corrupt, and the 14-bit NRPN scale is live
    //                     wire protocol for this family
    //   CC 0, 32          Bank Select, a transaction with the Program Change that follows
    //   CC 120-127        Channel Mode messages: events, not state
    //   0xF1              MIDI Time Code quarter-frames, each carrying a different nibble
    //                     index, so merging them destroys the timecode
    static bool coalescable(const MidiMessage& m)
    {
        const uint8_t type = (uint8_t)(m.status & 0xF0u);
        if (type == 0xE0u || type == 0xD0u) return true;    // pitch bend, channel pressure
        if (type != 0xB0u) return false;                     // notes and the rest are events

        const uint8_t c = m.data[0];
        if (c == 0u || c == 32u) return false;               // bank select
        if (c == 6u || c == 38u) return false;               // data entry
        if (c >= 96u && c <= 101u) return false;             // data inc/dec, RPN/NRPN select
        if (c >= 120u) return false;                         // channel mode
        return true;
    }

private:
    struct Entry {
        MidiMessage msg;
        Origin      origin = Origin::Forwarded;
    };

    // Two messages are the same thing being said again: the same controller on the same
    // channel, or the same channel's bend or pressure.
    static bool same_identity(const MidiMessage& a, const MidiMessage& b)
    {
        if (a.status != b.status) return false;
        return ((a.status & 0xF0u) == 0xB0u) ? (a.data[0] == b.data[0]) : true;
    }

    // What an admitted message costs, in the order the cheapest thing goes first.
    //
    // A coalescable entry is the cheapest to lose: it is a value whose sender will send
    // another, where a note or a program change is said once. Between two of equal standing
    // the pedal gives up a forwarded one before its own, and otherwise the oldest goes --
    // it has waited longest and is the most stale by the time it reaches the wire.
    uint16_t victim(Origin incoming) const
    {
        const bool spare_own = (incoming == Origin::Own);
        int32_t coalescable_any = -1, coalescable_fwd = -1, oldest_fwd = -1;

        for (uint16_t i = 0u; i < m_count; ++i) {
            const Entry& e = m_slot[index(i)];
            const bool forwarded = (e.origin == Origin::Forwarded);
            if (coalescable(e.msg)) {
                if (coalescable_any < 0) coalescable_any = (int32_t)i;
                if (forwarded && coalescable_fwd < 0) coalescable_fwd = (int32_t)i;
            }
            if (forwarded && oldest_fwd < 0) oldest_fwd = (int32_t)i;
        }

        if (spare_own && coalescable_fwd >= 0) return (uint16_t)coalescable_fwd;
        if (coalescable_any >= 0)              return (uint16_t)coalescable_any;
        if (spare_own && oldest_fwd >= 0)      return (uint16_t)oldest_fwd;
        return 0u;   // the oldest
    }

    // Lift one entry out and close the gap, so what remains keeps its order. Linear in the
    // depth, and reached only at saturation: an admitted message is rare where a refused one
    // is not.
    void evict(uint16_t nth)
    {
        for (uint16_t i = nth; (uint16_t)(i + 1u) < m_count; ++i)
            m_slot[index(i)] = m_slot[index((uint16_t)(i + 1u))];
        --m_count;
    }

    uint16_t index(uint16_t nth) const { return (uint16_t)((m_head + nth) % DEPTH); }

    Entry    m_slot[DEPTH];
    uint16_t m_head  = 0u;
    uint16_t m_count = 0u;
};

}  // namespace pedal_core
