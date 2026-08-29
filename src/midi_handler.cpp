#include <pedal_core/midi_handler.hpp>
#include <pedal_core/jack_router.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // SYSEX_RX_BUF

// Resolved at link time from the product's main.cpp (see the header). A
// product without a tempo layer or note handling supplies empty bodies for
// the last three.
extern "C" void on_midi_cc(uint8_t channel, uint8_t cc, uint8_t value);
extern "C" void on_midi_program_change(uint8_t channel, uint8_t program);
extern "C" void on_midi_sysex(const uint8_t* data, uint16_t len);
extern "C" void on_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

// Note off is weak, so the six callbacks a product already defines stay the whole
// obligation: one that wants note lifetimes -- holding a pitch while a key is down, and
// letting go when it is released -- overrides it, and every other product links unchanged.
// The empty default is in midi_note_off_default.cpp rather than here, because a test
// compiles this file into its own translation unit and supplies its own definition.
extern "C" void on_midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
extern "C" void on_midi_clock();
extern "C" void on_midi_clock_reset();

static midi_handler::Config s_config;

// The SysEx commands rx_sysex never blocks. Empty until the product names them.
static bool s_sysex_dispatched = false;   // one reply answered this pass; see update()

static const uint8_t* s_sysex_always     = nullptr;
static uint8_t        s_sysex_always_len = 0;

// The MIDI Out jack's arbiter: the lock a streaming frame takes, the queue a contending
// message waits in, the running-status hold, the stall timeout, and the policy deciding
// whether a source reaches a jack at all. It holds its own copy of the routing settings
// -- the fields it reads and the ones the parser below reads are disjoint -- so it can be
// stood up and driven on its own. See test_jack_router.
static pedal_core::JackRouter s_router;
using Src = pedal_core::JackRouter::Src;

// Running-status parser, one per transport so an interleaved USB message can
// never corrupt a half-received jack message.
struct Parser {
    uint8_t status = 0;
    uint8_t data[2];
    uint8_t data_len = 0;
    uint8_t data_idx = 0;

    // SysEx accumulation. The product's midi_protocol header sizes the buffer
    // clear of the largest frame it accepts.
    static constexpr uint16_t SYSEX_BUF = SYSEX_RX_BUF;
    uint8_t  sysex_buf[SYSEX_BUF];
    uint16_t sysex_len      = 0;
    bool     in_sysex       = false;
    bool     sysex_overflow = false;  // the frame outgrew the buffer
    bool     sysex_to_jack   = false;  // this frame won the MIDI jack and is streaming out
};

static Parser s_jack_parser;
static Parser s_usb_parser;

static uint8_t expected_data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        case 0xF0:
            switch (status) {
                case 0xF1: case 0xF3: return 1;
                case 0xF2: return 2;
                default: return 0;
            }
        default: return 0;
    }
}

static bool channel_matches(uint8_t msg_ch)
{
    return s_config.omni || (msg_ch == s_config.channel);
}

static void dispatch(uint8_t status, const uint8_t* data, uint8_t /*len*/)
{
    const uint8_t msg_type = status & 0xF0;
    const uint8_t msg_ch   = status & 0x0F;

    if (!channel_matches(msg_ch) && status < 0xF0) return;

    switch (msg_type) {
        case 0x90:
            if (data[1] > 0) { on_midi_note_on(msg_ch, data[0], data[1]); break; }
            [[fallthrough]];  // note on with vel=0 = note off
        case 0x80:
            // Both spellings of note off reach the same callback, so a product never has
            // to know which one a keyboard sends. data[1] is the release velocity, which
            // is 0 for the vel-0 note-on spelling.
            on_midi_note_off(msg_ch, data[0], data[1]);
            break;
        case 0xB0:
            on_midi_cc(msg_ch, data[0], data[1]);
            break;
        case 0xC0:
            // Rx PC off leaves the pedal on its own preset while one controller
            // sends program changes to the whole board.
            if (s_config.rx_pc) on_midi_program_change(msg_ch, data[0]);
            break;
        default:
            break;
    }

    // System real-time. Clock (0xF8) feeds the product's tempo layer, if it
    // has one. Start (0xFA) and Stop (0xFC) both reset the clock accumulator
    // and clear sync; Continue (0xFB) resumes at the same tempo, so it keeps
    // the tick ring rather than dropping sync.
    if (status == 0xF8) on_midi_clock();
    if (status == 0xFA || status == 0xFC) on_midi_clock_reset();
}

// Whether this frame reaches the product while rx_sysex is off. The command byte
// sits at index 3 of the family frame (F0 <mfr> <device> <cmd>); a frame too
// short to have one is not ours and is dropped either way.
static bool sysex_always_accepted(const Parser& p)
{
    if (p.sysex_len < 4u || s_sysex_always == nullptr) return false;
    for (uint8_t i = 0; i < s_sysex_always_len; ++i)
        if (s_sysex_always[i] == p.sysex_buf[3]) return true;
    return false;
}

static void dispatch_sysex(Parser& p)
{
    // The identity, query and re-enable commands answer even with SysEx receive
    // switched off, so the switch guards a pedal's presets without ever being a
    // lockout an editor cannot undo.
    const bool accept = !p.sysex_overflow
                     && (s_config.rx_sysex || sysex_always_accepted(p));
    if (accept && p.sysex_len >= 2) {
        on_midi_sysex(p.sysex_buf, p.sysex_len);
        s_sysex_dispatched = true;   // the pass has spent its reply budget
    }
    p.sysex_len      = 0;
    p.in_sysex       = false;
    p.sysex_overflow = false;
}

// The jack half is the router's, which decides for itself whether this source reaches
// the jack; the USB half is one write behind the same policy object.
static void route_realtime(Src src, uint8_t status)
{
    s_router.realtime(src, status);
    if (s_router.usb_carries(src) && status != 0xFEu) usb_midi::send(&status, 1);
}

static void route_message(Src src, const uint8_t* msg, uint16_t n)
{
    s_router.message(src, msg, n);
    if (s_router.usb_carries(src)) usb_midi::send(msg, (uint8_t)n);
}

static void feed_byte(Parser& p, uint8_t byte, Src src)
{
    // Real-time bytes can appear anywhere -- including mid-SysEx -- and have no
    // data bytes; handling them here keeps them out of the running-status
    // machinery and lets them pass the router's lock untouched, which is exactly
    // what the spec reserves them for.
    if (byte >= 0xF8) {
        route_realtime(src, byte);
        dispatch(byte, nullptr, 0);
        return;
    }

    if (byte == 0xF0) {
        p.in_sysex       = true;
        p.sysex_len      = 0;
        p.sysex_overflow = false;
        p.sysex_buf[p.sysex_len++] = byte;
        const uint32_t now = systick::now_ms();
        p.sysex_to_jack = s_router.carries(src) && s_router.sysex_begin(src, now);
        if (p.sysex_to_jack) s_router.sysex_byte(byte, now);
        return;
    }
    if (p.in_sysex) {
        if (byte == 0xF7) {                 // EOX -- complete and dispatch the SysEx
            if (p.sysex_len < Parser::SYSEX_BUF) p.sysex_buf[p.sysex_len++] = byte;
            else                                 p.sysex_overflow = true;
            if (p.sysex_to_jack) { s_router.sysex_end(src, true); p.sysex_to_jack = false; }
            // A frame is forwarded to the jack byte by byte but reaches USB whole, so one
            // that outgrew the receive buffer cannot be forwarded there.
            if (s_router.usb_carries(src) && !p.sysex_overflow)
                usb_midi::send_sysex(p.sysex_buf, p.sysex_len);
            dispatch_sysex(p);
            return;
        }
        if (byte < 0x80) {                  // SysEx data byte
            if (p.sysex_len < Parser::SYSEX_BUF) p.sysex_buf[p.sysex_len++] = byte;
            else                                 p.sysex_overflow = true;
            if (p.sysex_to_jack) s_router.sysex_byte(byte, systick::now_ms());
            return;
        }
        // Any other status byte aborts the SysEx (MIDI spec: only real-time may
        // appear inside SysEx, and those were already handled above). Close the
        // forwarded frame with an EOX so the downstream parser is not left holding
        // a frame that never ends, then fall through to parse this byte as a fresh
        // status rather than silently swallowing it and the rest of the message.
        if (p.sysex_to_jack) { s_router.sysex_end(src, true); p.sysex_to_jack = false; }
        p.in_sysex       = false;
        p.sysex_len      = 0;
        p.sysex_overflow = false;
    }

    if (byte & 0x80) {
        p.status    = byte;
        p.data_idx  = 0;
        p.data_len  = expected_data_bytes(byte);
        if (p.data_len == 0) {
            route_message(src, &byte, 1);
            dispatch(byte, nullptr, 0);
            p.status = 0;
        }
        return;
    }

    if (p.status == 0) return;  // stray data with no running status

    p.data[p.data_idx++] = byte;
    if (p.data_idx >= p.data_len) {
        // Handed on whole -- status byte included -- so the router knows what the
        // message is. Whether that status reaches the wire is the router's decision:
        // it holds running status when the jack is already on that status, so a
        // forwarded stream is never longer than the one that arrived.
        const uint8_t msg[3] = { p.status, p.data[0], p.data[1] };
        route_message(src, msg, (uint16_t)(1u + p.data_len));
        dispatch(p.status, p.data, p.data_len);
        p.data_idx = 0;
        // Running status persists for channel messages; system messages do
        // not use it.
        if (p.status >= 0xF0) p.status = 0;
    }
}

// Bring the whole module to a known state: the settings, both parsers, the router and the
// commands rx_sysex never blocks. A caller gets the same module from init() whatever came
// before it, which is what lets a suite start each case from one call.
void midi_handler::init(uint8_t channel, bool omni)
{
    s_config         = Config{};
    s_config.channel = channel;
    s_config.omni    = omni;

    s_jack_parser = Parser{};
    s_usb_parser  = Parser{};

    s_router = pedal_core::JackRouter{};
    s_router.set_config(s_config);

    s_sysex_always     = nullptr;
    s_sysex_always_len = 0;
    s_sysex_dispatched = false;
}

void midi_handler::update()
{
    // One reply in flight is the whole dispatch budget.
    //
    // A short message costs microseconds and lands in a queue that drops rather than blocks,
    // so it needs no budget. A SysEx reply does: it streams under the jack's frame lock, and
    // a host sending a burst of dump requests would otherwise have this call answer all of
    // them in one pass -- long enough for the product to miss its watchdog. The rest of the
    // burst stays in the receive ring and is answered on later passes.
    //
    // The budget is one dispatched frame, not one held lock: a reply is written whole rather
    // than streamed, so it never holds the jack across a pass and there is no lock to test
    // for. Bounding the count is what the budget was for either way.
    s_sysex_dispatched = false;

    uint8_t byte;
    while (uart::read(byte)) {
        feed_byte(s_jack_parser, byte, Src::Jack);
        if (s_sysex_dispatched) break;
    }
    while (usb_midi::read(byte) && !s_sysex_dispatched) {
        feed_byte(s_usb_parser, byte, Src::Usb);
    }

    // A frame that stopped coming has to give the jack back, or everything the
    // pedal says queues behind a sender that is no longer there. Close the
    // forwarded copy with an EOX so the downstream parser is not left holding a
    // frame that never ends, and stop forwarding the rest of it.
    //
    // The local parse is deliberately left alone: a host that merely paused still
    // gets its frame understood when it resumes. Only the pass-through copy --
    // already truncated downstream by the silence -- is abandoned.
    Src stalled_owner = Src::Jack;
    if (s_router.poll(systick::now_ms(), stalled_owner)) {
        Parser& p = (stalled_owner == Src::Usb) ? s_usb_parser : s_jack_parser;
        p.sysex_to_jack = false;
    }

    // Send what the wire has room for. Nothing here waits: the queue holds what the ring
    // cannot take yet.
    s_router.pump();
}

// The router reads the routing fields and the parser below reads the receive ones, and the
// two sets are disjoint -- so a change to the receive channel is none of the jack's
// business. Telling it anyway would have it forget the status byte on the wire, re-stating
// one for a change the jack never saw.
void midi_handler::set_channel(uint8_t ch) { s_config.channel = ch; }
void midi_handler::set_omni(bool omni)     { s_config.omni = omni; }
void midi_handler::set_config(const Config& cfg)
{
    s_config = cfg;
    // The router forgets the status byte on the wire as it takes the new settings; see
    // JackRouter::set_config() for why a routing change has to re-state it.
    s_router.set_config(s_config);
}
const midi_handler::Config& midi_handler::get_config() { return s_config; }

uint8_t midi_handler::tx_channel()
{
    return (s_config.tx_channel <= 15u) ? s_config.tx_channel : s_config.channel;
}

void midi_handler::send_own(const uint8_t* msg, uint16_t len)
{
    s_router.message(Src::Self, msg, len);
}

void midi_handler::send_own_realtime(uint8_t status)
{
    s_router.realtime(Src::Self, status);
}

void midi_handler::set_sysex_always_accepted(const uint8_t* cmds, uint8_t count)
{
    s_sysex_always     = cmds;
    s_sysex_always_len = count;
}

void midi_handler::set_generating_clock(bool on) { s_router.set_generating_clock(on); }
