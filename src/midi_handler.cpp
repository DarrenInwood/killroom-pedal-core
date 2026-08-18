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

static midi_handler::Config s_config = { 0, false };

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
    uint16_t sysex_len    = 0;
    bool     in_sysex     = false;
};

static Parser s_uart_parser;
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
            on_midi_program_change(msg_ch, data[0]);
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

static void dispatch_sysex(Parser& p)
{
    if (p.sysex_len >= 2) {
        on_midi_sysex(p.sysex_buf, p.sysex_len);
    }
    p.sysex_len = 0;
    p.in_sysex  = false;
}

static void feed_byte(Parser& p, uint8_t byte, bool is_uart)
{
    // MIDI Thru: every hardware byte is echoed before parsing, so a message
    // this pedal does not understand still reaches whatever is downstream.
    if (is_uart) uart::write(byte);

    // Real-time bytes can appear anywhere — including mid-SysEx — and have no
    // data bytes; dispatching here keeps them out of the running-status
    // machinery.
    if (byte >= 0xF8) {
        dispatch(byte, nullptr, 0);
        return;
    }

    if (byte == 0xF0) {
        p.in_sysex  = true;
        p.sysex_len = 0;
        p.sysex_buf[p.sysex_len++] = byte;
        return;
    }
    if (p.in_sysex) {
        if (byte == 0xF7) {                 // EOX — complete and dispatch the SysEx
            if (p.sysex_len < Parser::SYSEX_BUF)
                p.sysex_buf[p.sysex_len++] = byte;
            dispatch_sysex(p);
            return;
        }
        if (byte < 0x80) {                  // SysEx data byte
            if (p.sysex_len < Parser::SYSEX_BUF)
                p.sysex_buf[p.sysex_len++] = byte;
            return;
        }
        // Any other status byte aborts the SysEx (MIDI spec: only real-time may
        // appear inside SysEx, and those were already handled above). Discard the
        // incomplete SysEx and fall through to parse this byte as a fresh status,
        // rather than silently swallowing it and the rest of the message.
        p.in_sysex  = false;
        p.sysex_len = 0;
    }

    if (byte & 0x80) {
        p.status    = byte;
        p.data_idx  = 0;
        p.data_len  = expected_data_bytes(byte);
        if (p.data_len == 0) dispatch(byte, nullptr, 0);
        return;
    }

    if (p.status == 0) return;  // stray data with no running status

    p.data[p.data_idx++] = byte;
    if (p.data_idx >= p.data_len) {
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
    while (uart::read(byte))     feed_byte(s_uart_parser, byte, true);
    while (usb_midi::read(byte)) feed_byte(s_usb_parser,  byte, false);
}

void midi_handler::set_channel(uint8_t ch) { s_config.channel = ch; }
void midi_handler::set_omni(bool omni)     { s_config.omni = omni; }
const midi_handler::Config& midi_handler::get_config() { return s_config; }
