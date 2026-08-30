#pragma once
#include <cstdint>
#include "hal.hpp"           // uart::write
#include "midi_handler.hpp"   // Config, OutMode, UsbJackRoute
#include "midi_out_queue.hpp" // the transmit queue and what survives it

// The MIDI Out router: what arbitrates the MIDI Out jack between the three things that
// want it -- the inbound stream being echoed, the other transport being cross-routed,
// and the pedal's own traffic.
//
// It exists because the MIDI spec reserves System Real-Time for appearing anywhere in
// the stream, including inside a SysEx frame, and reserves nothing else. Everything else
// has to leave the jack whole, so a frame streaming through takes a lock and any complete
// message from another source waits in a queue behind it rather than splicing into it.
//
// It knows no clock: every entry point that needs the time is given it. It writes to one
// place, uart::write, and decides rather than performs everything else -- so a suite
// stands the whole module up behind a recording writer, with no parser upstream and no
// byte stream to fake.
namespace pedal_core {

class JackRouter {
public:
    // Which stream a byte belongs to. `Self` is the pedal's own outbound traffic, which
    // contends for the jack exactly as the two inbound streams contend with each other.
    enum class Src : uint8_t { Jack, Usb, Self };

    // Which port the question is about. This module writes only the jack; the USB port is
    // written by the caller. Whether a source reaches one is the same question as whether
    // it reaches the other, so both are answered here rather than half here and half at
    // the call site.
    enum class Port : uint8_t { Jack, Usb };

    // The settings this module reads, and only those.
    //
    // Deliberately not the whole of midi_handler::Config. The receive half -- channel,
    // omni, rx_pc, rx_sysex -- decides what the pedal ACTS on, which is not a question
    // about the jack, and holding a copy of it here is what would let a rule read a
    // channel that set_channel() had already moved on from. The two sets are disjoint in
    // the type rather than in a comment.
    //
    // tx_params is not here either, and is not carries-policy. It separates one KIND of
    // the pedal's own traffic from another -- an unasked-for echo of a knob the player
    // just moved, against a message the product meant to send or a reply a host asked for
    // -- and Src::Self cannot tell those apart. It also gates both transports rather than
    // a route. It stays with MidiResponderBase, which knows which of the two it holds.
    struct RoutingPolicy {
        midi_handler::OutMode      out_mode   = midi_handler::OutMode::Merge;
        midi_handler::UsbJackRoute usb_jack   = midi_handler::UsbJackRoute::Off;
        bool                       clock_thru = true;
    };

    // The routing settings. Also forgets which status byte is on the wire: running status
    // is a claim about what the device downstream has already been told, and a routing
    // change can mean it was told nothing -- the jack having been Off, or carrying another
    // source -- so the next message re-states its status. Re-stating one is never wrong,
    // only occasionally a byte that was not strictly needed.
    void set_policy(const RoutingPolicy& p)
    {
        m_policy  = p;
        m_running = 0u;
    }

    // The same, from the whole of a product's configuration: the routing fields are taken
    // and the receive ones left where they belong.
    void set_config(const midi_handler::Config& cfg)
    {
        set_policy(RoutingPolicy{ cfg.out_mode, cfg.usb_jack, cfg.clock_thru });
    }

    // Whether the pedal is generating its own clock. midi_clock_out keeps this current.
    void set_generating_clock(bool on) { m_generating_clock = on; }

    // --- carries-policy -------------------------------------------------------
    // Pure predicates over the settings, and part of the interface: these are the rules
    // that surprise people, and a truth table is the shape they read in. Everything below
    // consults them, so they are never the whole of what a suite covers.

    // Does this source's traffic reach this port, for the current routing?
    //
    // The whole table, and the only one: a caller that writes to a port it does not own
    // asks this rather than keeping a clause of its own. `status` is part of the question
    // because two of the rules are about what the message IS rather than where it came
    // from -- Active Sensing goes nowhere, and the clock family rides its own switch. Pass
    // a frame's opening F0; pass 0 where the question is only about the source.
    bool carries(Src src, Port port, uint8_t status) const
    {
        using midi_handler::OutMode;

        // Active Sensing describes one link, not the stream on it: forwarding it makes the
        // device on the far side start expecting a heartbeat this pedal is not promising
        // to keep. Dropped on every setting, and on both ports.
        if (status == 0xFEu) return false;

        if (port == Port::Usb) {
            // The cross-route is a different job from the echo -- the pedal standing in as
            // a MIDI interface -- and it carries what arrives on the jack whatever the jack
            // itself is set to do with it.
            //
            // The clock rules below are the jack's alone, and deliberately so: clock_thru
            // names a thru, and "exactly one clock leaves the jack" is an invariant about
            // the wire this module writes. A host watching an inbound clock over USB is not
            // a second clock on anybody's chain.
            return src == Src::Jack && jack_reaches_usb();
        }

        // An inbound System Real-Time byte. The pedal's own is judged by whether the jack
        // carries its traffic at all, not by the echo's rules: clock_thru governs
        // forwarding somebody else's clock, and a pedal generating one must not suppress
        // its own -- so Src::Self falls through to the table below.
        if (status >= 0xF8u && src != Src::Self) {
            if (m_policy.out_mode == OutMode::Off) return false;
            if (src == Src::Usb && !usb_reaches_jack()) return false;

            // The clock family rides its own switch, so a pedal can be the tempo master
            // for the chain below it while still listening to a clock above. While the
            // pedal is generating, the inbound clock is dropped whatever that switch says
            // -- two clocks on one wire read as neither.
            if (status == 0xF8u || status == 0xFAu || status == 0xFBu || status == 0xFCu)
                return m_policy.clock_thru && !m_generating_clock;

            // System Reset is a panic message; it travels with the echo.
            return echoes();
        }

        switch (src) {
            case Src::Jack: return echoes();
            case Src::Usb:  return echoes() && usb_reaches_jack();
            case Src::Self:
            default:        return m_policy.out_mode == OutMode::Merge
                                || m_policy.out_mode == OutMode::Out;
        }
    }

    // Does this source's traffic reach the MIDI jack at all, setting aside what the
    // message is? The question most callers have, and the same table underneath.
    bool carries(Src src) const { return carries(src, Port::Jack, 0u); }

    // Does an inbound System Real-Time byte reach the MIDI jack? Named for the case that
    // reads oddly without a name, and answered by the same table.
    bool carries_realtime(Src src, uint8_t status) const
    {
        return carries(src, Port::Jack, status);
    }

    // --- traffic ---------------------------------------------------------------

    // One complete message the jack should carry: a channel message, or a whole F0..F7
    // frame. Dropped where the policy does not carry this source.
    //
    // A short message joins the transmit queue, which decides what survives when there is
    // more to send than 31.25 kbaud can carry -- see MidiOutQueue. A frame cannot: it has no
    // identity to coalesce on and no bound on its length, so it keeps the older path of
    // waiting whole for the jack.
    void message(Src src, const uint8_t* msg, uint16_t n)
    {
        if (n == 0u || !carries(src)) return;

        if (n > 3u || msg[0] == 0xF0u) {
            // A frame leaves whole or not at all, so it waits for three things: the jack,
            // when another frame owns it; any frame already queued, so the order the sender
            // chose survives; and room in the ring for the whole of it. That last one is
            // what uart::tx_room() is for -- a frame begun with room for half of it reaches
            // the receiver as a dump it cannot tell from a good one.
            const bool waiting = (m_locked && m_owner != src)
                              || m_queued > 0u
                              || uart::tx_room() < frame_length(msg, n);
            if (waiting) queue_push(msg, n);
            else         emit(msg, n);
            if (!m_locked) queue_flush();
            return;
        }

        MidiMessage m;
        m.status = msg[0];
        m.len    = (uint8_t)n;
        for (uint8_t i = 1u; i < (uint8_t)n; ++i) m.data[i - 1u] = msg[i];
        m_out.push(m, (src == Src::Self) ? MidiOutQueue::Origin::Own
                                         : MidiOutQueue::Origin::Forwarded);
        pump();
    }

    // Send what the wire has room for. Call every superloop wake, and after anything that
    // frees the jack.
    //
    // Nothing here waits: the queue holds what the ring cannot take yet, and the ring is
    // asked for room before a message is begun rather than during it. A frame streaming
    // through owns the jack outright, so the queue holds until it ends.
    void pump()
    {
        if (m_locked) return;

        // Frames first, then short messages, which is the order they were given in: a frame
        // only ever waits here because the jack or the ring was busy, and it was handed over
        // before anything still sitting in the transmit queue.
        queue_flush();

        MidiMessage next;
        while (m_out.peek(next)) {
            if (uart::tx_room() < wire_length(next)) return;
            m_out.pop(next);
            emit_message(next);
        }
    }

    // What the queue is holding, for a caller that wants to know whether the jack is behind.
    uint16_t pending() const { return m_out.size(); }

    // One System Real-Time byte. Legal anywhere in the stream, so it is never queued and
    // passes a streaming frame untouched.
    //
    // The pedal's own is judged by whether the jack carries its traffic at all, not by
    // the echo's rules: clock_thru governs forwarding somebody else's clock, and a pedal
    // generating one must not suppress its own.
    //
    // No room is asked for here either: one byte cannot be half-written, so what a full ring
    // cannot take is already lost whole. That is the right loss for this family -- a clock
    // byte held back until there is room would arrive late, which is worse than not at all.
    void realtime(Src src, uint8_t status)
    {
        if (carries(src, Port::Jack, status)) uart::write(status);
    }

    // --- a frame streaming through ---------------------------------------------

    // Claim the jack for a frame about to stream byte by byte. A frame can be any length
    // -- a firmware image passing down the chain is why it streams rather than buffers --
    // so a second frame arriving mid-stream is refused outright: the jack cannot carry
    // both, and 31250 baud could not fit them anyway.
    bool sysex_begin(Src src, uint32_t now_ms)
    {
        if (m_locked) return false;
        m_locked  = true;
        m_owner   = src;
        m_fed_ms  = now_ms;
        m_running = 0u;          // SysEx ends whatever run was on the wire
        return true;
    }

    // One byte of the frame that holds the jack, which also says the frame is alive.
    //
    // This is the one write that does not ask the ring for room, and cannot. A frame streams
    // because it has no bound on its length and nowhere to be held whole, and its F0 is
    // already downstream: dropping a byte from the middle truncates a frame the receiver is
    // part-way through parsing, which is the outcome asking for room exists to avoid. Both
    // jacks run at 31250 baud, so a frame arriving cannot outrun the copy leaving.
    void sysex_byte(uint8_t b, uint32_t now_ms)
    {
        uart::write(b);
        m_fed_ms = now_ms;
    }

    // Release the jack, and let whatever queued behind the frame out.
    void sysex_end(Src src, bool write_eox)
    {
        if (!m_locked || m_owner != src) return;
        if (write_eox) uart::write(0xF7u);
        m_locked = false;
        pump();   // the frames that waited for the jack, then the transmit queue
    }

    // Has the frame holding the jack stopped coming?
    //
    // A jack sender is a UART pushing bytes 320 us apart and a host's packets are far
    // closer together than this, so only a frame that has stopped trips it -- an unplugged
    // cable mid-dump being the case that matters. Without it the lock outlives the frame
    // and the pedal's own output is silent until a fresh status byte arrives on that
    // stream, which never happens if the cable is out.
    bool stalled(uint32_t now_ms) const
    {
        return m_locked && (uint32_t)(now_ms - m_fed_ms) >= STALL_MS;
    }

    // Take the jack back from a frame that has stopped coming, and say whether it did.
    // Call every superloop wake.
    //
    // The forwarded copy is closed with an EOX so the downstream parser is not left
    // holding a frame that never ends, and whatever queued behind it goes out. `owner`
    // names the stream the frame belonged to: the caller has a parser of its own to stop
    // forwarding from, and that is its state rather than the jack's.
    bool poll(uint32_t now_ms, Src& owner)
    {
        if (!stalled(now_ms)) return false;
        owner = m_owner;
        sysex_end(m_owner, /*write_eox=*/true);
        return true;
    }

private:
    static constexpr uint16_t QUEUE_BYTES = 128;  // one preset dump, comfortably
    static constexpr uint32_t STALL_MS    = 1000u;

    // The three clauses carries() is built from, named so the table above reads as the
    // rules rather than as the settings they happen to be spelled in.
    bool echoes() const
    {
        return m_policy.out_mode == midi_handler::OutMode::Merge
            || m_policy.out_mode == midi_handler::OutMode::Thru;
    }
    bool usb_reaches_jack() const
    {
        return m_policy.usb_jack == midi_handler::UsbJackRoute::UsbToJack
            || m_policy.usb_jack == midi_handler::UsbJackRoute::Both;
    }
    bool jack_reaches_usb() const
    {
        return m_policy.usb_jack == midi_handler::UsbJackRoute::JackToUsb
            || m_policy.usb_jack == midi_handler::UsbJackRoute::Both;
    }

    // A queued message as the byte run it is on the wire. The queue holds canonical
    // messages; everything that writes one or measures one wants the bytes, so the two
    // shapes meet here rather than in each of them.
    static uint16_t flatten(const MidiMessage& m, uint8_t (&out)[3])
    {
        out[0] = m.status;
        for (uint8_t i = 1u; i < m.len; ++i) out[i] = m.data[i - 1u];
        return m.len;
    }

    void raw(const uint8_t* b, uint16_t n)
    {
        for (uint16_t i = 0; i < n; ++i) uart::write(b[i]);
    }

    // How many bytes this message costs on the wire right now -- one fewer when its status
    // is already the one the jack is on. Asked before the message is begun, because a
    // message half-written is a message the receiver cannot parse.
    uint16_t wire_length(const MidiMessage& m) const
    {
        uint8_t b[3];
        const uint16_t n = flatten(m, b);
        return frame_length(b, n);
    }

    // The same question for a frame that emit() is about to write: how many bytes it costs
    // on the wire right now, one fewer when its status is already the one the jack is on.
    // Kept beside wire_length() because both answer for the writer they precede, and the
    // two writers spell running status the same way.
    uint16_t frame_length(const uint8_t* msg, uint16_t n) const
    {
        if (n == 0u) return 0u;
        const uint8_t st = msg[0];
        const bool running = (st >= 0x80u && st < 0xF0u && st == m_running);
        return running ? (uint16_t)(n - 1u) : n;
    }

    // One queued message onto the jack. Coalescing and eviction rewrite the queue after a
    // message enters it, so which status byte is already on the wire is not knowable until
    // here -- the queue holds canonical messages, and the wire representation is emit()'s.
    // Running status is held there rather than a second time here: one writer, one rule.
    void emit_message(const MidiMessage& m)
    {
        uint8_t b[3];
        const uint16_t n = flatten(m, b);
        emit(b, n);
    }

    // One whole message onto the jack, holding running status: a channel message whose
    // status is already the one on the wire goes out as its data bytes alone, exactly as
    // the controller sent it.
    //
    // Holding it is not an optimisation. Both jacks run at 31250 baud, so a forwarded
    // stream longer than the one arriving cannot be sustained at all, and re-emitting a
    // status byte per message costs 50% on a saturated NRPN stream -- 50% more than the
    // wire has. There is nowhere to borrow that from: what the jack cannot carry is lost
    // a whole message at a time -- a short one by the transmit queue's rules, which are
    // MidiOutQueue's to state, and a frame dropped entire by queue_push() rather than
    // truncated. The status bytes not sent are the margin that keeps messages out of that
    // reckoning.
    //
    // System Common and SysEx break the run (the spec says so); System Real-Time does not,
    // which is why realtime() writes straight past this.
    void emit(const uint8_t* msg, uint16_t n)
    {
        if (n == 0u) return;
        const uint8_t st = msg[0];
        if (st >= 0x80u && st < 0xF0u) {
            if (st == m_running) { raw(msg + 1, (uint16_t)(n - 1)); return; }
            m_running = st;
        } else if (st >= 0xF0u && st <= 0xF7u) {
            m_running = 0u;
        }
        raw(msg, n);
    }

    // A whole frame or nothing: one the queue cannot hold is dropped rather than truncated,
    // because half a frame downstream is worse than none. Records are length-prefixed so the
    // flush still knows where each ends -- it has to, or it could not decide which status
    // bytes running status lets it leave out.
    //
    // This holds frames only. Short messages go to the transmit queue instead, where they
    // can coalesce and where what is lost under pressure is chosen rather than arbitrary.
    void queue_push(const uint8_t* b, uint16_t n)
    {
        if (n > 255u) return;
        if ((uint16_t)(n + 1u) > (uint16_t)(QUEUE_BYTES - m_queued)) return;
        m_queue[m_queued++] = (uint8_t)n;
        for (uint16_t i = 0; i < n; ++i) m_queue[m_queued++] = b[i];
    }

    // Let the waiting frames out, oldest first, for as long as the ring can take the whole
    // of the next one. One that will not fit yet stays queued with everything behind it, so
    // the order holds and no frame is begun that the wire cannot finish; the next pump()
    // tries again. Nothing here waits on the wire.
    void queue_flush()
    {
        uint16_t i = 0;
        while (i < m_queued) {
            const uint8_t n = m_queue[i];
            if (uart::tx_room() < frame_length(&m_queue[i + 1u], n)) break;
            emit(&m_queue[i + 1u], n);
            i = (uint16_t)(i + 1u + n);
        }
        if (i == 0u) return;

        // Close the gap the flushed frames left, so the queue stays packed from the front.
        for (uint16_t j = i; j < m_queued; ++j) m_queue[j - i] = m_queue[j];
        m_queued = (uint16_t)(m_queued - i);
    }

    RoutingPolicy m_policy{};
    bool     m_generating_clock = false;

    bool     m_locked  = false;
    Src      m_owner   = Src::Jack;
    // The last channel status actually written to the jack.
    uint8_t  m_running = 0u;
    uint32_t m_fed_ms  = 0u;     // when the owning frame last produced a byte

    uint8_t  m_queue[QUEUE_BYTES] = {};   // frames waiting for the jack
    uint16_t m_queued = 0u;

    MidiOutQueue m_out;                   // short messages waiting for the wire
};

}  // namespace pedal_core
