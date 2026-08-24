#include <pedal_core/midi_handler.hpp>
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

using midi_handler::OutMode;
using midi_handler::UsbDinRoute;

static midi_handler::Config s_config;

// The SysEx commands rx_sysex never blocks. Empty until the product names them.
static const uint8_t* s_sysex_always     = nullptr;
static uint8_t        s_sysex_always_len = 0;

// Set by midi_clock_out while it is actually ticking.
static bool s_generating_clock = false;

// Which stream a byte belongs to. `Self` is the pedal's own outbound traffic,
// which contends for the DIN jack with an inbound echo exactly as the two
// inbound streams contend with each other.
enum class Src : uint8_t { Uart, Usb, Self };

// Running-status parser, one per transport so an interleaved USB message can
// never corrupt a half-received DIN message.
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
    bool     sysex_to_din   = false;  // this frame won the DIN jack and is streaming out
};

static Parser s_uart_parser;
static Parser s_usb_parser;

// ---------------------------------------------------------------------------
// The DIN Out router
//
// The MIDI spec reserves System Real-Time for appearing anywhere in the stream,
// including inside a SysEx frame. Everything else has to leave the jack whole,
// so a frame streaming out takes a lock and any complete message from another
// source waits in the queue behind it.
// ---------------------------------------------------------------------------
namespace {

constexpr uint16_t DIN_QUEUE_BYTES = 128;  // one preset dump, comfortably

// How long a streaming frame may go quiet before the jack is taken back. A DIN
// sender is a UART pushing bytes 320 us apart and a host's packets are far closer
// together than this, so only a frame that has stopped coming trips it -- an
// unplugged cable mid-dump being the case that matters. Without it the lock
// outlives the frame and the pedal's own output is silent until a fresh status
// byte arrives on that stream, which never happens if the cable is out.
constexpr uint32_t DIN_STALL_MS = 1000u;

bool     s_din_locked = false;
Src      s_din_owner  = Src::Uart;
// The last channel status actually written to the jack. Forwarding a stream that
// holds running status is not an optimisation here: both jacks run at 31250 baud,
// so a forwarded stream longer than the one arriving cannot be sustained at all --
// the transmit ring fills, uart::write spins, the loop stops draining the receive
// ring, and messages are lost. Re-emitting a status byte per message costs 50% on a
// saturated NRPN stream, which is 50% more than the wire has.
uint8_t  s_din_running = 0;
uint32_t s_din_fed_ms = 0;      // when the owning frame last produced a byte
uint8_t  s_din_queue[DIN_QUEUE_BYTES];
uint16_t s_din_queued = 0;

void din_raw(const uint8_t* b, uint16_t n)
{
    for (uint16_t i = 0; i < n; ++i) uart::write(b[i]);
}

void din_queue_flush();   // defined below din_emit, which it forwards each record to

// Whole message or nothing: a message the queue cannot hold is dropped rather
// than truncated, because half a message downstream is worse than none. Records are
// length-prefixed so the flush still knows where each message ends -- it has to, or
// it could not decide which status bytes running status lets it leave out.
void din_queue_push(const uint8_t* b, uint16_t n)
{
    if (n > 255u) return;
    if ((uint16_t)(n + 1u) > (uint16_t)(DIN_QUEUE_BYTES - s_din_queued)) return;
    s_din_queue[s_din_queued++] = (uint8_t)n;
    for (uint16_t i = 0; i < n; ++i) s_din_queue[s_din_queued++] = b[i];
}

// One whole message onto the jack, holding running status: a channel message whose
// status is already the one on the wire goes out as its data bytes alone, exactly as
// the controller sent it. System Common and SysEx break the run (the spec says so);
// System Real-Time does not, which is why route_realtime writes straight past this.
void din_emit(const uint8_t* msg, uint16_t n)
{
    if (n == 0u) return;
    const uint8_t st = msg[0];
    if (st >= 0x80u && st < 0xF0u) {
        if (st == s_din_running) { din_raw(msg + 1, (uint16_t)(n - 1)); return; }
        s_din_running = st;
    } else if (st >= 0xF0u && st <= 0xF7u) {
        s_din_running = 0;
    }
    din_raw(msg, n);
}

// Does this source's traffic reach the DIN jack at all?
bool din_carries(Src src)
{
    switch (src) {
        case Src::Uart:
            return s_config.out_mode == OutMode::Merge || s_config.out_mode == OutMode::Thru;
        case Src::Usb:
            return (s_config.out_mode == OutMode::Merge || s_config.out_mode == OutMode::Thru)
                && (s_config.usb_din == UsbDinRoute::UsbToDin || s_config.usb_din == UsbDinRoute::Both);
        case Src::Self:
        default:
            return s_config.out_mode == OutMode::Merge || s_config.out_mode == OutMode::Out;
    }
}

void din_queue_flush()
{
    uint16_t i = 0;
    while (i < s_din_queued) {
        const uint8_t n = s_din_queue[i++];
        din_emit(&s_din_queue[i], n);
        i = (uint16_t)(i + n);
    }
    s_din_queued = 0;
}

void din_message(Src src, const uint8_t* msg, uint16_t n)
{
    if (s_din_locked && s_din_owner != src) { din_queue_push(msg, n); return; }
    din_emit(msg, n);
    if (!s_din_locked) din_queue_flush();
}

// Claim the jack for a SysEx frame about to stream through byte by byte. A frame
// can be any length -- a firmware image passing down the chain is why it streams
// rather than buffers -- so a second frame arriving mid-stream is refused
// outright: the jack cannot carry both, and 31250 baud could not fit them anyway.
bool din_sysex_begin(Src src)
{
    if (s_din_locked) return false;
    s_din_locked  = true;
    s_din_owner   = src;
    s_din_fed_ms  = systick::now_ms();
    s_din_running = 0;      // SysEx ends whatever run was on the wire
    return true;
}

// One byte of the frame that holds the jack, which also says the frame is alive.
void din_sysex_byte(uint8_t b)
{
    uart::write(b);
    s_din_fed_ms = systick::now_ms();
}

void din_sysex_end(Src src, bool write_eox)
{
    if (!s_din_locked || s_din_owner != src) return;
    if (write_eox) uart::write(0xF7u);
    s_din_locked = false;
    din_queue_flush();
}

// Has the frame holding the jack stopped coming?
bool din_stalled(uint32_t now_ms)
{
    return s_din_locked && (uint32_t)(now_ms - s_din_fed_ms) >= DIN_STALL_MS;
}

// Does an inbound System Real-Time byte reach the DIN jack?
bool din_carries_realtime(Src src, uint8_t status)
{
    if (s_config.out_mode == OutMode::Off) return false;
    if (src == Src::Usb
        && s_config.usb_din != UsbDinRoute::UsbToDin && s_config.usb_din != UsbDinRoute::Both)
        return false;

    // Active Sensing describes one link, not the stream on it: forwarding it
    // makes a downstream device start expecting a heartbeat this pedal is not
    // promising to keep. Dropped on every setting.
    if (status == 0xFEu) return false;

    // The clock family rides its own switch, so a pedal can be the tempo master
    // for the chain below it while still listening to a clock above. While the
    // pedal is generating, the inbound clock is dropped whatever that switch says
    // -- two clocks on one wire read as neither.
    if (status == 0xF8u || status == 0xFAu || status == 0xFBu || status == 0xFCu)
        return s_config.clock_thru && !s_generating_clock;

    // System Reset is a panic message; it travels with the echo.
    return s_config.out_mode == OutMode::Merge || s_config.out_mode == OutMode::Thru;
}

bool usb_carries(Src src)
{
    return src == Src::Uart
        && (s_config.usb_din == UsbDinRoute::DinToUsb || s_config.usb_din == UsbDinRoute::Both);
}

}  // namespace

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
    }
    p.sysex_len      = 0;
    p.in_sysex       = false;
    p.sysex_overflow = false;
}

static void route_realtime(Src src, uint8_t status)
{
    if (din_carries_realtime(src, status)) uart::write(status);
    if (usb_carries(src) && status != 0xFEu) usb_midi::send(&status, 1);
}

static void route_message(Src src, const uint8_t* msg, uint16_t n)
{
    if (din_carries(src)) din_message(src, msg, n);
    if (usb_carries(src)) usb_midi::send(msg, (uint8_t)n);
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
        p.sysex_to_din = din_carries(src) && din_sysex_begin(src);
        if (p.sysex_to_din) din_sysex_byte(byte);
        return;
    }
    if (p.in_sysex) {
        if (byte == 0xF7) {                 // EOX -- complete and dispatch the SysEx
            if (p.sysex_len < Parser::SYSEX_BUF) p.sysex_buf[p.sysex_len++] = byte;
            else                                 p.sysex_overflow = true;
            if (p.sysex_to_din) { din_sysex_end(src, true); p.sysex_to_din = false; }
            // A frame is forwarded to DIN byte by byte but reaches USB whole, so one
            // that outgrew the receive buffer cannot be forwarded there.
            if (usb_carries(src) && !p.sysex_overflow)
                usb_midi::send_sysex(p.sysex_buf, p.sysex_len);
            dispatch_sysex(p);
            return;
        }
        if (byte < 0x80) {                  // SysEx data byte
            if (p.sysex_len < Parser::SYSEX_BUF) p.sysex_buf[p.sysex_len++] = byte;
            else                                 p.sysex_overflow = true;
            if (p.sysex_to_din) din_sysex_byte(byte);
            return;
        }
        // Any other status byte aborts the SysEx (MIDI spec: only real-time may
        // appear inside SysEx, and those were already handled above). Close the
        // forwarded frame with an EOX so the downstream parser is not left holding
        // a frame that never ends, then fall through to parse this byte as a fresh
        // status rather than silently swallowing it and the rest of the message.
        if (p.sysex_to_din) { din_sysex_end(src, true); p.sysex_to_din = false; }
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
        // message is. Whether that status reaches the wire is din_emit's decision:
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

void midi_handler::init(uint8_t channel, bool omni)
{
    s_config.channel = channel;
    s_config.omni    = omni;
}

void midi_handler::update()
{
    uint8_t byte;
    while (uart::read(byte))     feed_byte(s_uart_parser, byte, Src::Uart);
    while (usb_midi::read(byte)) feed_byte(s_usb_parser,  byte, Src::Usb);

    // A frame that stopped coming has to give the jack back, or everything the
    // pedal says queues behind a sender that is no longer there. Close the
    // forwarded copy with an EOX so the downstream parser is not left holding a
    // frame that never ends, and stop forwarding the rest of it.
    //
    // The local parse is deliberately left alone: a host that merely paused still
    // gets its frame understood when it resumes. Only the pass-through copy --
    // already truncated downstream by the silence -- is abandoned.
    if (din_stalled(systick::now_ms())) {
        Parser& p = (s_din_owner == Src::Usb) ? s_usb_parser : s_uart_parser;
        p.sysex_to_din = false;
        din_sysex_end(s_din_owner, true);
    }
}

void midi_handler::set_channel(uint8_t ch) { s_config.channel = ch; }
void midi_handler::set_omni(bool omni)     { s_config.omni = omni; }
void midi_handler::set_config(const Config& cfg)
{
    s_config = cfg;
    // Forget which status byte is on the wire. Running status is a claim about what the
    // device downstream has already been told, and a routing change can mean it was told
    // nothing -- the jack having been Off, or carrying another source -- so the next
    // message re-states its status. Re-stating one is never wrong, only occasionally a
    // byte that was not strictly needed.
    s_din_running = 0;
}
const midi_handler::Config& midi_handler::get_config() { return s_config; }

uint8_t midi_handler::tx_channel()
{
    return (s_config.tx_channel <= 15u) ? s_config.tx_channel : s_config.channel;
}

void midi_handler::send_own(const uint8_t* msg, uint16_t len)
{
    if (len == 0 || !din_carries(Src::Self)) return;
    din_message(Src::Self, msg, len);
}

void midi_handler::send_own_realtime(uint8_t status)
{
    if (!din_carries(Src::Self)) return;
    uart::write(status);
}

void midi_handler::set_sysex_always_accepted(const uint8_t* cmds, uint8_t count)
{
    s_sysex_always     = cmds;
    s_sysex_always_len = count;
}

void midi_handler::set_generating_clock(bool on) { s_generating_clock = on; }
