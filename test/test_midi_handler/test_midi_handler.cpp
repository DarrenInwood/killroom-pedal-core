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
    static std::deque<uint8_t> g_rx;
    bool read(uint8_t& b) { if (g_rx.empty()) return false; b = g_rx.front(); g_rx.pop_front(); return true; }
    // init/task/send/send_sysex are not referenced by midi_handler.cpp
}

// --- recording stubs for the dispatch callbacks ----------------------------
struct CcMsg   { uint8_t ch, cc, val; };
struct PcMsg   { uint8_t ch, prog; };
struct NoteMsg { uint8_t ch, note, vel; };
static std::vector<CcMsg>                g_cc;
static std::vector<PcMsg>                g_pc;
static std::vector<NoteMsg>              g_note;
static std::vector<std::vector<uint8_t>> g_sysex;
static int g_clock       = 0;
static int g_clock_reset = 0;

extern "C" void on_midi_cc(uint8_t ch, uint8_t cc, uint8_t v)      { g_cc.push_back({ch, cc, v}); }
extern "C" void on_midi_program_change(uint8_t ch, uint8_t p)      { g_pc.push_back({ch, p}); }
extern "C" void on_midi_sysex(const uint8_t* d, uint16_t len)      { g_sysex.emplace_back(d, d + len); }
extern "C" void on_midi_note_on(uint8_t ch, uint8_t n, uint8_t v)  { g_note.push_back({ch, n, v}); }
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
    g_sysex.clear();
    g_clock = g_clock_reset = 0;
    // Reset the file-static parser state (visible here because we #include the .cpp)
    // so running status / SysEx accumulation never leaks across tests.
    s_uart_parser = Parser{};
    s_usb_parser  = Parser{};
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
    feed_uart({0x90, 0x3C, 0x64});   // note on, vel 100 -> dispatched
    TEST_ASSERT_EQUAL_INT(1, (int)g_note.size());
    TEST_ASSERT_EQUAL_UINT8(0x3C, g_note[0].note);
    TEST_ASSERT_EQUAL_UINT8(0x64, g_note[0].vel);
    feed_uart({0x90, 0x3C, 0x00});   // note on, vel 0 == note off -> NOT dispatched
    TEST_ASSERT_EQUAL_INT(1, (int)g_note.size());
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
    TEST_ASSERT_EQUAL_INT(1, (int)g_sysex.size());
    TEST_ASSERT_EQUAL_INT(512, (int)g_sysex[0].size());
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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cc_dispatch_on_matching_channel);
    RUN_TEST(test_cc_dropped_on_non_matching_channel);
    RUN_TEST(test_omni_accepts_any_channel);
    RUN_TEST(test_running_status_repeats_last_status);
    RUN_TEST(test_note_on_and_velocity_zero_is_note_off);
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
    return UNITY_END();
}
