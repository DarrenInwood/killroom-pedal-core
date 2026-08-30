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

    // The routing settings. Also forgets which status byte is on the wire: running status
    // is a claim about what the device downstream has already been told, and a routing
    // change can mean it was told nothing -- the jack having been Off, or carrying another
    // source -- so the next message re-states its status. Re-stating one is never wrong,
    // only occasionally a byte that was not strictly needed.
    void set_config(const midi_handler::Config& cfg)
    {
        m_config  = cfg;
        m_running = 0u;
    }

    // Whether the pedal is generating its own clock. midi_clock_out keeps this current.
    void set_generating_clock(bool on) { m_generating_clock = on; }

    // --- carries-policy -------------------------------------------------------
    // Pure predicates over the settings, and part of the interface: these are the rules
    // that surprise people, and a truth table is the shape they read in. Everything below
    // consults them, so they are never the whole of what a suite covers.

    // Does this source's traffic reach the MIDI jack at all?
    bool carries(Src src) const
    {
        using midi_handler::OutMode;
        using midi_handler::UsbJackRoute;
        switch (src) {
            case Src::Jack:
                return m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Thru;
            case Src::Usb:
                return (m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Thru)
                    && (m_config.usb_jack == UsbJackRoute::UsbToJack
                        || m_config.usb_jack == UsbJackRoute::Both);
            case Src::Self:
            default:
                return m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Out;
        }
    }

    // Does an inbound System Real-Time byte reach the MIDI jack?
    bool carries_realtime(Src src, uint8_t status) const
    {
        using midi_handler::OutMode;
        using midi_handler::UsbJackRoute;

        if (m_config.out_mode == OutMode::Off) return false;
        if (src == Src::Usb
            && m_config.usb_jack != UsbJackRoute::UsbToJack
            && m_config.usb_jack != UsbJackRoute::Both)
            return false;

        // Active Sensing describes one link, not the stream on it: forwarding it makes a
        // downstream device start expecting a heartbeat this pedal is not promising to
        // keep. Dropped on every setting.
        if (status == 0xFEu) return false;

        // The clock family rides its own switch, so a pedal can be the tempo master for
        // the chain below it while still listening to a clock above. While the pedal is
        // generating, the inbound clock is dropped whatever that switch says -- two clocks
        // on one wire read as neither.
        if (status == 0xF8u || status == 0xFAu || status == 0xFBu || status == 0xFCu)
            return m_config.clock_thru && !m_generating_clock;

        // System Reset is a panic message; it travels with the echo.
        return m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Thru;
    }

    // Does this source's traffic reach the USB port? The cross-route is the same decision
    // as the one above and is answered here so the whole table lives in one module, even
    // though the writing is the caller's.
    bool usb_carries(Src src) const
    {
        using midi_handler::UsbJackRoute;
        return src == Src::Jack
            && (m_config.usb_jack == UsbJackRoute::JackToUsb
                || m_config.usb_jack == UsbJackRoute::Both);
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
            if (m_locked && m_owner != src) { queue_push(msg, n); return; }
            emit(msg, n);
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
        MidiMessage next;
        while (!m_locked && m_out.peek(next)) {
            if (uart::tx_room() < wire_length(next)) return;
            m_out.pop(next);
            emit_message(next);
        }
    }

    // What the queue is holding, for a caller that wants to know whether the jack is behind.
    uint16_t pending() const { return m_out.size(); }

    // Whether a frame from this source is streaming through right now. A caller uses it to
    // stop handing itself more work while a reply of its own is still going out.
    bool streaming_from(Src src) const { return m_locked && m_owner == src; }

    // One System Real-Time byte. Legal anywhere in the stream, so it is never queued and
    // passes a streaming frame untouched.
    //
    // The pedal's own is judged by whether the jack carries its traffic at all, not by
    // the echo's rules: clock_thru governs forwarding somebody else's clock, and a pedal
    // generating one must not suppress its own.
    void realtime(Src src, uint8_t status)
    {
        const bool ok = (src == Src::Self) ? carries(src) : carries_realtime(src, status);
        if (ok) uart::write(status);
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
        queue_flush();   // whole frames that waited for the jack
        pump();          // then whatever the transmit queue is holding
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

    void raw(const uint8_t* b, uint16_t n)
    {
        for (uint16_t i = 0; i < n; ++i) uart::write(b[i]);
    }

    // How many bytes this message costs on the wire right now -- one fewer when its status
    // is already the one the jack is on. Asked before the message is begun, because a
    // message half-written is a message the receiver cannot parse.
    uint16_t wire_length(const MidiMessage& m) const
    {
        const uint8_t st = m.status;
        const bool running = (st >= 0x80u && st < 0xF0u && st == m_running);
        return running ? (uint16_t)(m.len - 1u) : (uint16_t)m.len;
    }

    // One queued message onto the jack, holding running status. Coalescing and eviction
    // rewrite the queue after a message enters it, so which status byte is already on the
    // wire is not knowable until here: the queue holds canonical messages and this owns the
    // wire representation.
    void emit_message(const MidiMessage& m)
    {
        const uint8_t st = m.status;
        if (st >= 0x80u && st < 0xF0u) {
            if (st == m_running) {
                for (uint8_t i = 1u; i < m.len; ++i) uart::write(m.data[i - 1u]);
                return;
            }
            m_running = st;
        } else if (st >= 0xF0u && st <= 0xF7u) {
            m_running = 0u;
        }
        uart::write(st);
        for (uint8_t i = 1u; i < m.len; ++i) uart::write(m.data[i - 1u]);
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

    void queue_flush()
    {
        uint16_t i = 0;
        while (i < m_queued) {
            const uint8_t n = m_queue[i++];
            emit(&m_queue[i], n);
            i = (uint16_t)(i + n);
        }
        m_queued = 0u;
    }

    midi_handler::Config m_config{};
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
