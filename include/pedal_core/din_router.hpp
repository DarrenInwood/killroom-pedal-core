#pragma once
#include <cstdint>
#include "hal.hpp"           // uart::write
#include "midi_handler.hpp"  // Config, OutMode, UsbDinRoute

// The DIN Out router: what arbitrates the DIN Out jack between the three things that
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

class DinRouter {
public:
    // Which stream a byte belongs to. `Self` is the pedal's own outbound traffic, which
    // contends for the jack exactly as the two inbound streams contend with each other.
    enum class Src : uint8_t { Uart, Usb, Self };

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

    // Does this source's traffic reach the DIN jack at all?
    bool carries(Src src) const
    {
        using midi_handler::OutMode;
        using midi_handler::UsbDinRoute;
        switch (src) {
            case Src::Uart:
                return m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Thru;
            case Src::Usb:
                return (m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Thru)
                    && (m_config.usb_din == UsbDinRoute::UsbToDin
                        || m_config.usb_din == UsbDinRoute::Both);
            case Src::Self:
            default:
                return m_config.out_mode == OutMode::Merge || m_config.out_mode == OutMode::Out;
        }
    }

    // Does an inbound System Real-Time byte reach the DIN jack?
    bool carries_realtime(Src src, uint8_t status) const
    {
        using midi_handler::OutMode;
        using midi_handler::UsbDinRoute;

        if (m_config.out_mode == OutMode::Off) return false;
        if (src == Src::Usb
            && m_config.usb_din != UsbDinRoute::UsbToDin
            && m_config.usb_din != UsbDinRoute::Both)
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
        using midi_handler::UsbDinRoute;
        return src == Src::Uart
            && (m_config.usb_din == UsbDinRoute::DinToUsb
                || m_config.usb_din == UsbDinRoute::Both);
    }

    // --- traffic ---------------------------------------------------------------

    // One complete message (a channel message, or a whole F0..F7 frame). Dropped where
    // the policy does not carry this source. Contending with a frame already streaming it
    // waits in the queue; a message that does not fit the queue is dropped rather than
    // spliced.
    void message(Src src, const uint8_t* msg, uint16_t n)
    {
        if (n == 0u || !carries(src)) return;
        if (m_locked && m_owner != src) { queue_push(msg, n); return; }
        emit(msg, n);
        if (!m_locked) queue_flush();
    }

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
        queue_flush();
    }

    // Has the frame holding the jack stopped coming?
    //
    // A DIN sender is a UART pushing bytes 320 us apart and a host's packets are far
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

    // One whole message onto the jack, holding running status: a channel message whose
    // status is already the one on the wire goes out as its data bytes alone, exactly as
    // the controller sent it.
    //
    // Holding it is not an optimisation. Both jacks run at 31250 baud, so a forwarded
    // stream longer than the one arriving cannot be sustained at all -- the transmit ring
    // fills, uart::write spins, the loop stops draining the receive ring, and messages are
    // lost. Re-emitting a status byte per message costs 50% on a saturated NRPN stream,
    // which is 50% more than the wire has.
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

    // Whole message or nothing: one the queue cannot hold is dropped rather than
    // truncated, because half a message downstream is worse than none. Records are
    // length-prefixed so the flush still knows where each message ends -- it has to, or
    // it could not decide which status bytes running status lets it leave out.
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
    Src      m_owner   = Src::Uart;
    // The last channel status actually written to the jack.
    uint8_t  m_running = 0u;
    uint32_t m_fed_ms  = 0u;     // when the owning frame last produced a byte

    uint8_t  m_queue[QUEUE_BYTES] = {};
    uint16_t m_queued = 0u;
};

}  // namespace pedal_core
