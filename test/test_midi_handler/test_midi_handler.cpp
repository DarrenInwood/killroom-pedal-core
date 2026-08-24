// Host-native unit tests for the MIDI parser (src/midi_handler.cpp).
//
// midi_handler.cpp is a CMSIS-free running-status / SysEx parser that drains the
// uart + usb_midi byte streams and dispatches to the on_midi_* callbacks. Like the
// driver tests, we compile it into this TU via #include behind recording stubs for
// the two drivers it reads and the six callbacks it invokes. The parser owns subtle,
// regression-prone behaviour — running status, mid-message real-time bytes, SysEx
// accumulation/abort/bounds, note-on vel=0 == note-off, and channel filtering — none
// of which had any coverage before this suite.

#include <unity.h>
#include <cstdint>
#include <deque>
#include <vector>

// --- recording stubs for the driver reads/writes midi_handler.cpp calls -----
namespace uart {
    static std::deque<uint8_t>  g_rx;
    static std::vector<uint8_t> g_thru;   // bytes echoed by MIDI Thru
    void init() {}
    bool read(uint8_t& b) { if (g_rx.empty()) return false; b = g_rx.front(); g_rx.pop_front(); return true; }
    void write(uint8_t b) { g_thru.push_back(b); }
}
namespace usb_midi {
    static std::deque<uint8_t>  g_rx;
    static std::vector<uint8_t> g_tx;         // messages the router put on USB
    static std::vector<std::vector<uint8_t>> g_tx_sysex;
    bool read(uint8_t& b) { if (g_rx.empty()) return false; b = g_rx.front(); g_rx.pop_front(); return true; }
    void send(const uint8_t* m, uint8_t n) { g_tx.insert(g_tx.end(), m, m + n); }
    void send_sysex(const uint8_t* f, uint16_t n) { g_tx_sysex.emplace_back(f, f + n); }
    // init/task are not referenced by midi_handler.cpp
}

// --- recording stubs for the dispatch callbacks ----------------------------
struct CcMsg   { uint8_t ch, cc, val; };
struct PcMsg   { uint8_t ch, prog; };
struct NoteMsg { uint8_t ch, note, vel; };
static std::vector<CcMsg>                g_cc;
static std::vector<PcMsg>                g_pc;
static std::vector<NoteMsg>              g_note;
static std::vector<NoteMsg>              g_note_off;
static std::vector<std::vector<uint8_t>> g_sysex;
static int g_clock       = 0;
static int g_clock_reset = 0;

extern "C" void on_midi_cc(uint8_t ch, uint8_t cc, uint8_t v)      { g_cc.push_back({ch, cc, v}); }
extern "C" void on_midi_program_change(uint8_t ch, uint8_t p)      { g_pc.push_back({ch, p}); }
extern "C" void on_midi_sysex(const uint8_t* d, uint16_t len)      { g_sysex.emplace_back(d, d + len); }
extern "C" void on_midi_note_on(uint8_t ch, uint8_t n, uint8_t v)  { g_note.push_back({ch, n, v}); }
extern "C" void on_midi_note_off(uint8_t ch, uint8_t n, uint8_t v) { g_note_off.push_back({ch, n, v}); }
extern "C" void on_midi_clock()                                    { ++g_clock; }
extern "C" void on_midi_clock_reset()                              { ++g_clock_reset; }

// Pull in the implementation under test (after the stubs/callbacks it depends on).
#include "../../src/midi_handler.cpp"

// Feed bytes into the UART RX stream and run one parser pass.
static void feed_uart(std::initializer_list<int> bytes) {
    for (int b : bytes) uart::g_rx.push_back((uint8_t)b);
    midi_handler::update();
}
static void feed_usb(std::initializer_list<int> bytes) {
    for (int b : bytes) usb_midi::g_rx.push_back((uint8_t)b);
    midi_handler::update();
}

void setUp(void) {
    uart::g_rx.clear();
    uart::g_thru.clear();
    usb_midi::g_rx.clear();
    g_cc.clear();
    g_pc.clear();
    g_note.clear();
    g_note_off.clear();
    g_sysex.clear();
    g_clock = g_clock_reset = 0;
    // Reset the file-static parser state (visible here because we #include the .cpp)
    // so running status / SysEx accumulation never leaks across tests.
    s_uart_parser = Parser{};
    s_usb_parser  = Parser{};
    // The DIN router's lock and queue are file-static too, and a test that leaves a
    // SysEx half-forwarded would otherwise hand the lock to the next one.
    usb_midi::g_tx.clear();
    usb_midi::g_tx_sysex.clear();
    s_din_locked = false;
    s_din_queued = 0;
    s_sysex_always     = nullptr;
    s_sysex_always_len = 0;
    midi_handler::set_config(midi_handler::Config{});   // every default is today's behaviour
    midi_handler::init(0, false);   // channel 0, omni off
}
void tearDown(void) {}

void test_cc_dispatch_on_matching_channel(void) {
    feed_uart({0xB0, 0x07, 0x40});   // CC#7 = 64 on channel 0
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());
    TEST_ASSERT_EQUAL_UINT8(0,    g_cc[0].ch);
    TEST_ASSERT_EQUAL_UINT8(0x07, g_cc[0].cc);
    TEST_ASSERT_EQUAL_UINT8(0x40, g_cc[0].val);
}

void test_cc_dropped_on_non_matching_channel(void) {
    feed_uart({0xB1, 0x07, 0x40});   // channel 1, config is channel 0, omni off
    TEST_ASSERT_EQUAL_INT(0, (int)g_cc.size());
}

void test_omni_accepts_any_channel(void) {
    midi_handler::set_omni(true);
    feed_uart({0xB5, 0x0A, 0x7F});   // channel 5
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());
    TEST_ASSERT_EQUAL_UINT8(5, g_cc[0].ch);
}

void test_running_status_repeats_last_status(void) {
    // One B0 status then three (cc,val) pairs with no repeated status byte.
    feed_uart({0xB0, 0x01, 0x10, 0x02, 0x20, 0x03, 0x30});
    TEST_ASSERT_EQUAL_INT(3, (int)g_cc.size());
    TEST_ASSERT_EQUAL_UINT8(0x01, g_cc[0].cc);  TEST_ASSERT_EQUAL_UINT8(0x10, g_cc[0].val);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_cc[1].cc);  TEST_ASSERT_EQUAL_UINT8(0x20, g_cc[1].val);
    TEST_ASSERT_EQUAL_UINT8(0x03, g_cc[2].cc);  TEST_ASSERT_EQUAL_UINT8(0x30, g_cc[2].val);
}

void test_note_on_and_velocity_zero_is_note_off(void) {
    feed_uart({0x90, 0x3C, 0x64});   // note on, vel 100 -> dispatched as note on
    TEST_ASSERT_EQUAL_INT(1, (int)g_note.size());
    TEST_ASSERT_EQUAL_UINT8(0x3C, g_note[0].note);
    TEST_ASSERT_EQUAL_UINT8(0x64, g_note[0].vel);
    feed_uart({0x90, 0x3C, 0x00});   // note on, vel 0 == note off -> the note-off callback
    TEST_ASSERT_EQUAL_INT(1, (int)g_note.size());
    TEST_ASSERT_EQUAL_INT(1, (int)g_note_off.size());
    TEST_ASSERT_EQUAL_UINT8(0x3C, g_note_off[0].note);
}

void test_program_change_single_data_byte(void) {
    feed_uart({0xC0, 0x07});
    TEST_ASSERT_EQUAL_INT(1, (int)g_pc.size());
    TEST_ASSERT_EQUAL_UINT8(0x07, g_pc[0].prog);
}

void test_realtime_clock_mid_message_does_not_corrupt(void) {
    // F8 (clock) arrives between the two data bytes of a CC; it must dispatch the
    // clock and leave the partial CC intact so the CC still completes correctly.
    feed_uart({0xB0, 0x07, 0xF8, 0x40});
    TEST_ASSERT_EQUAL_INT(1, g_clock);
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());
    TEST_ASSERT_EQUAL_UINT8(0x07, g_cc[0].cc);
    TEST_ASSERT_EQUAL_UINT8(0x40, g_cc[0].val);
}

void test_realtime_clock_inside_sysex_passes_through(void) {
    // A realtime clock (F8) embedded mid-SysEx must be dispatched immediately and NOT
    // buffered into the SysEx payload; the SysEx still completes with F8 removed.
    feed_uart({0xF0, 0x11, 0xF8, 0x22, 0xF7});
    TEST_ASSERT_EQUAL_INT(1, g_clock);
    TEST_ASSERT_EQUAL_INT(1, (int)g_sysex.size());
    TEST_ASSERT_EQUAL_INT(4, (int)g_sysex[0].size());  // F0 11 22 F7 (F8 not buffered)
}

void test_clock_start_and_stop_reset(void) {
    // Start (0xFA) and Stop (0xFC) both clear the clock sync accumulator, matching
    // tap_tempo::midi_clock_reset()'s "call on MIDI FA/FC (start/stop)" contract.
    feed_uart({0xFA});   // Start
    feed_uart({0xFC});   // Stop
    TEST_ASSERT_EQUAL_INT(2, g_clock_reset);
}

void test_clock_continue_does_not_reset(void) {
    // Continue (0xFB) resumes at the same tempo, so it must NOT drop sync: only
    // Start/Stop reset the accumulator.
    feed_uart({0xFB});   // Continue
    TEST_ASSERT_EQUAL_INT(0, g_clock_reset);
}

void test_sysex_accumulates_and_dispatches(void) {
    feed_uart({0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7});
    TEST_ASSERT_EQUAL_INT(1, (int)g_sysex.size());
    // Delivered buffer includes the leading F0 and trailing F7.
    const std::vector<uint8_t> expect = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7};
    TEST_ASSERT_EQUAL_INT((int)expect.size(), (int)g_sysex[0].size());
    for (size_t i = 0; i < expect.size(); ++i)
        TEST_ASSERT_EQUAL_UINT8(expect[i], g_sysex[0][i]);
}

void test_sysex_aborted_by_status_byte(void) {
    // A non-realtime status byte mid-SysEx aborts it (no dispatch) and is then
    // parsed as a fresh status — the following CC must complete normally.
    feed_uart({0xF0, 0x11, 0x12, 0xB0, 0x20, 0x30});
    TEST_ASSERT_EQUAL_INT(0, (int)g_sysex.size());
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());
    TEST_ASSERT_EQUAL_UINT8(0x20, g_cc[0].cc);
    TEST_ASSERT_EQUAL_UINT8(0x30, g_cc[0].val);
}

void test_sysex_buffer_is_bounded(void) {
    // Far more than the 512-byte SysEx buffer: must not overflow, and the delivered
    // length is clamped to the buffer size.
    uart::g_rx.push_back(0xF0);
    for (int i = 0; i < 1000; ++i) uart::g_rx.push_back(0x01);
    uart::g_rx.push_back(0xF7);
    midi_handler::update();
    // A frame that outgrew the buffer is dropped, not handed on truncated: the
    // bytes past the bound are gone, so what is left is a different frame, and a
    // product would reject it one layer down for having no closing F7 anyway.
    TEST_ASSERT_EQUAL_INT(0, (int)g_sysex.size());
    // It still reaches the jack in full, because forwarding streams rather than buffers.
    TEST_ASSERT_EQUAL_INT(1002, (int)uart::g_thru.size());
}

void test_midi_thru_echoes_uart_but_not_usb(void) {
    feed_uart({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());   // all three hardware bytes echoed
    TEST_ASSERT_EQUAL_UINT8(0xB0, uart::g_thru[0]);
    TEST_ASSERT_EQUAL_UINT8(0x07, uart::g_thru[1]);
    TEST_ASSERT_EQUAL_UINT8(0x40, uart::g_thru[2]);

    uart::g_thru.clear();
    g_cc.clear();
    feed_usb({0xB0, 0x07, 0x40});                         // USB MIDI is not echoed to Thru
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());           // but it still dispatches
}

void test_bare_data_byte_without_status_is_ignored(void) {
    feed_uart({0x40, 0x50});   // no running status established yet
    TEST_ASSERT_EQUAL_INT(0, (int)g_cc.size());
    TEST_ASSERT_EQUAL_INT(0, (int)g_note.size());
}

// The 0x80 spelling reaches the same callback, carrying its release velocity, so a
// product tracking note lifetimes never has to know which spelling a keyboard sends.
void test_note_off_status_byte_dispatches(void) {
    feed_uart({0x80, 0x40, 0x20});
    TEST_ASSERT_EQUAL_INT(0, (int)g_note.size());
    TEST_ASSERT_EQUAL_INT(1, (int)g_note_off.size());
    TEST_ASSERT_EQUAL_UINT8(0x40, g_note_off[0].note);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_note_off[0].vel);
}

// ---------------------------------------------------------------------------
// The DIN Out router
// ---------------------------------------------------------------------------

// Merge is the default and carries both directions; the other three each drop
// one of them, which is the whole point of the control.
void test_out_mode_thru_drops_the_pedals_own_messages(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.out_mode = midi_handler::OutMode::Thru;
    midi_handler::set_config(c);

    feed_uart({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());   // the echo still runs

    uart::g_thru.clear();
    const uint8_t own[3] = {0xB0, 0x0F, 0x02};
    midi_handler::send_own(own, 3);
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());   // but the pedal stays silent
}

void test_out_mode_out_drops_the_echo(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.out_mode = midi_handler::OutMode::Out;
    midi_handler::set_config(c);

    feed_uart({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());           // still acted on locally

    const uint8_t own[3] = {0xB0, 0x0F, 0x02};
    midi_handler::send_own(own, 3);
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());
}

void test_out_mode_off_silences_the_jack(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.out_mode = midi_handler::OutMode::Off;
    midi_handler::set_config(c);

    feed_uart({0xB0, 0x07, 0x40, 0xF8});
    const uint8_t own[3] = {0xB0, 0x0F, 0x02};
    midi_handler::send_own(own, 3);
    midi_handler::send_own_realtime(0xF8);
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
}

// The reason messages are forwarded whole rather than byte by byte: a frame in
// flight owns the jack, and anything else waits behind it.
void test_own_message_waits_behind_an_inbound_sysex(void) {
    feed_uart({0xF0, 0x7D, 0x01});             // a frame starts and does not finish
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());

    const uint8_t own[3] = {0xB0, 0x0F, 0x02};
    midi_handler::send_own(own, 3);
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());   // held, not spliced in

    feed_uart({0xF7});
    // EOX first, then the message that was waiting.
    TEST_ASSERT_EQUAL_INT(7, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_UINT8(0xF7, uart::g_thru[3]);
    TEST_ASSERT_EQUAL_UINT8(0xB0, uart::g_thru[4]);
    TEST_ASSERT_EQUAL_UINT8(0x0F, uart::g_thru[5]);
    TEST_ASSERT_EQUAL_UINT8(0x02, uart::g_thru[6]);
}

// Real-time is the one thing the spec lets through a frame, so the lock ignores it.
void test_own_realtime_passes_through_an_inbound_sysex(void) {
    feed_uart({0xF0, 0x7D});
    midi_handler::send_own_realtime(0xF8);
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_UINT8(0xF8, uart::g_thru[2]);
}

// Running status is expanded on the way out, so each forwarded message is whole.
void test_running_status_is_expanded_when_forwarded(void) {
    feed_uart({0xB0, 0x01, 0x10, 0x02, 0x20});
    TEST_ASSERT_EQUAL_INT(6, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_UINT8(0xB0, uart::g_thru[3]);
    TEST_ASSERT_EQUAL_UINT8(0x02, uart::g_thru[4]);
    TEST_ASSERT_EQUAL_UINT8(0x20, uart::g_thru[5]);
}

void test_clock_thru_off_stops_forwarding_the_clock_family(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.clock_thru = false;
    midi_handler::set_config(c);

    feed_uart({0xF8, 0xFA, 0xFC, 0xFB});
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
    TEST_ASSERT_EQUAL_INT(1, g_clock);          // still drives the tempo layer
    TEST_ASSERT_EQUAL_INT(2, g_clock_reset);
}

// Active Sensing describes one link, so it never travels -- on any setting.
void test_active_sensing_is_never_forwarded(void) {
    feed_uart({0xFE});
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
}

// ---------------------------------------------------------------------------
// Cross-routing: the pedal as a MIDI interface
// ---------------------------------------------------------------------------
void test_din_to_usb_forwards_messages_and_frames(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.usb_din = midi_handler::UsbDinRoute::DinToUsb;
    midi_handler::set_config(c);

    feed_uart({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(3, (int)usb_midi::g_tx.size());

    feed_uart({0xF0, 0x7D, 0x01, 0x70, 0xF7});
    TEST_ASSERT_EQUAL_INT(1, (int)usb_midi::g_tx_sysex.size());
    TEST_ASSERT_EQUAL_INT(5, (int)usb_midi::g_tx_sysex[0].size());
}

void test_usb_to_din_needs_the_route_and_an_echoing_out_mode(void) {
    feed_usb({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());   // Off by default

    midi_handler::Config c = midi_handler::get_config();
    c.usb_din = midi_handler::UsbDinRoute::UsbToDin;
    midi_handler::set_config(c);
    feed_usb({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(3, (int)uart::g_thru.size());

    // OutMode::Out carries only the pedal's own traffic, so the crossing stops.
    uart::g_thru.clear();
    c.out_mode = midi_handler::OutMode::Out;
    midi_handler::set_config(c);
    feed_usb({0xB0, 0x07, 0x40});
    TEST_ASSERT_EQUAL_INT(0, (int)uart::g_thru.size());
}

// ---------------------------------------------------------------------------
// Receive filters
// ---------------------------------------------------------------------------
void test_rx_pc_off_drops_program_change_but_forwards_it(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.rx_pc = false;
    midi_handler::set_config(c);

    feed_uart({0xC0, 0x05});
    TEST_ASSERT_EQUAL_INT(0, (int)g_pc.size());
    TEST_ASSERT_EQUAL_INT(2, (int)uart::g_thru.size());   // the board below still gets it
}

void test_rx_sysex_off_drops_frames_but_keeps_the_named_commands(void) {
    static const uint8_t always[] = { 0x70u, 0x21u };
    midi_handler::set_sysex_always_accepted(always, 2);
    midi_handler::Config c = midi_handler::get_config();
    c.rx_sysex = false;
    midi_handler::set_config(c);

    feed_uart({0xF0, 0x7D, 0x01, 0x12, 0x00, 0xF7});      // a preset write: refused
    TEST_ASSERT_EQUAL_INT(0, (int)g_sysex.size());

    feed_uart({0xF0, 0x7D, 0x01, 0x70, 0xF7});            // identity: always answered
    TEST_ASSERT_EQUAL_INT(1, (int)g_sysex.size());
    TEST_ASSERT_EQUAL_UINT8(0x70, g_sysex[0][3]);
}

// ---------------------------------------------------------------------------
// The transmit channel
// ---------------------------------------------------------------------------
void test_tx_channel_follows_rx_until_one_is_set(void) {
    midi_handler::set_channel(4);
    TEST_ASSERT_EQUAL_UINT8(4, midi_handler::tx_channel());

    midi_handler::Config c = midi_handler::get_config();
    c.tx_channel = 9;
    midi_handler::set_config(c);
    TEST_ASSERT_EQUAL_UINT8(9, midi_handler::tx_channel());
}

// The case Omni gets wrong wherever it is one setting with the channel: a pedal
// listening to every channel still has exactly one it speaks on.
void test_omni_does_not_move_the_transmit_channel(void) {
    midi_handler::Config c = midi_handler::get_config();
    c.channel    = 6;
    c.omni       = true;
    c.tx_channel = midi_handler::TX_CHANNEL_FOLLOW_RX;
    midi_handler::set_config(c);
    TEST_ASSERT_EQUAL_UINT8(6, midi_handler::tx_channel());

    c.tx_channel = 2;
    midi_handler::set_config(c);
    TEST_ASSERT_EQUAL_UINT8(2, midi_handler::tx_channel());

    feed_uart({0xB9, 0x07, 0x40});                        // still hears channel 9
    TEST_ASSERT_EQUAL_INT(1, (int)g_cc.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cc_dispatch_on_matching_channel);
    RUN_TEST(test_cc_dropped_on_non_matching_channel);
    RUN_TEST(test_omni_accepts_any_channel);
    RUN_TEST(test_running_status_repeats_last_status);
    RUN_TEST(test_note_on_and_velocity_zero_is_note_off);
    RUN_TEST(test_note_off_status_byte_dispatches);
    RUN_TEST(test_program_change_single_data_byte);
    RUN_TEST(test_realtime_clock_mid_message_does_not_corrupt);
    RUN_TEST(test_realtime_clock_inside_sysex_passes_through);
    RUN_TEST(test_clock_start_and_stop_reset);
    RUN_TEST(test_clock_continue_does_not_reset);
    RUN_TEST(test_sysex_accumulates_and_dispatches);
    RUN_TEST(test_sysex_aborted_by_status_byte);
    RUN_TEST(test_sysex_buffer_is_bounded);
    RUN_TEST(test_midi_thru_echoes_uart_but_not_usb);
    RUN_TEST(test_bare_data_byte_without_status_is_ignored);
    RUN_TEST(test_out_mode_thru_drops_the_pedals_own_messages);
    RUN_TEST(test_out_mode_out_drops_the_echo);
    RUN_TEST(test_out_mode_off_silences_the_jack);
    RUN_TEST(test_own_message_waits_behind_an_inbound_sysex);
    RUN_TEST(test_own_realtime_passes_through_an_inbound_sysex);
    RUN_TEST(test_running_status_is_expanded_when_forwarded);
    RUN_TEST(test_clock_thru_off_stops_forwarding_the_clock_family);
    RUN_TEST(test_active_sensing_is_never_forwarded);
    RUN_TEST(test_din_to_usb_forwards_messages_and_frames);
    RUN_TEST(test_usb_to_din_needs_the_route_and_an_echoing_out_mode);
    RUN_TEST(test_rx_pc_off_drops_program_change_but_forwards_it);
    RUN_TEST(test_rx_sysex_off_drops_frames_but_keeps_the_named_commands);
    RUN_TEST(test_tx_channel_follows_rx_until_one_is_set);
    RUN_TEST(test_omni_does_not_move_the_transmit_channel);
    return UNITY_END();
}
