// Host-native unit tests for the USB-MIDI CIN classification (usb_midi_cin.hpp).
//
// The full usb_midi.cpp driver pulls in TinyUSB and so can't build on the host,
// but its correctness-critical packet<->stream mapping was extracted into the
// dependency-free usb_midi_cin.hpp. These tests lock both directions against the
// USB-MIDI 1.0 CIN table — including the Channel Pressure / Program Change split
// that pass-1 slot 11 had to fix (PC and ChPress are distinct 2-byte CINs).

#include <unity.h>
#include <cstdint>
#include <pedal_core/usb_midi_cin.hpp>
#include <pedal_core/vbus_debounce.hpp>

using namespace usb_midi_cin;

void setUp(void) {}
void tearDown(void) {}

// RX: single-byte CINs (0x5 System Common / SysEx-end-1, 0xF Single Byte) -> 1.
void test_rx_single_byte_cins(void) {
    TEST_ASSERT_EQUAL_UINT8(1, rx_data_count(0x05));
    TEST_ASSERT_EQUAL_UINT8(1, rx_data_count(0x0F));
}

// RX: the four 2-byte CINs -> 2 (incl. Program Change 0xC and Channel Pressure 0xD).
void test_rx_two_byte_cins(void) {
    TEST_ASSERT_EQUAL_UINT8(2, rx_data_count(0x02));
    TEST_ASSERT_EQUAL_UINT8(2, rx_data_count(0x06));
    TEST_ASSERT_EQUAL_UINT8(2, rx_data_count(0x0C));
    TEST_ASSERT_EQUAL_UINT8(2, rx_data_count(0x0D));
}

// RX: everything else (channel voice, SysEx start/continue, 3-byte sys-common) -> 3.
void test_rx_three_byte_cins(void) {
    const uint8_t threes[] = {0x03, 0x04, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0E};
    for (uint8_t c : threes)
        TEST_ASSERT_EQUAL_UINT8(3, rx_data_count(c));
}

// RX: the cable nibble (high bits of packet byte 0) is ignored.
void test_rx_ignores_cable_nibble(void) {
    TEST_ASSERT_EQUAL_UINT8(2, rx_data_count(0x9C));   // cable 9, CIN 0xC
    TEST_ASSERT_EQUAL_UINT8(1, rx_data_count(0x3F));   // cable 3, CIN 0xF
}

// TX 1-byte: Tune Request (0xF6) is System Common (0x5); real-time -> Single Byte (0xF).
void test_tx_one_byte(void) {
    TEST_ASSERT_EQUAL_UINT8(0x05, tx_cin(0xF6, 1));    // Tune Request
    TEST_ASSERT_EQUAL_UINT8(0x0F, tx_cin(0xF8, 1));    // Timing Clock (real-time)
    TEST_ASSERT_EQUAL_UINT8(0x0F, tx_cin(0xFA, 1));    // Start
}

// TX 2-byte: Program Change / Channel Pressure carry their own CIN; the slot-11
// fix — they must NOT both collapse to 0x0C.
void test_tx_two_byte_channel_voice_split(void) {
    TEST_ASSERT_EQUAL_UINT8(0x0C, tx_cin(0xC3, 2));    // Program Change, ch 3
    TEST_ASSERT_EQUAL_UINT8(0x0D, tx_cin(0xD7, 2));    // Channel Pressure, ch 7
}

// TX 2-byte: any other 2-byte message is System Common (0x2).
void test_tx_two_byte_system_common(void) {
    TEST_ASSERT_EQUAL_UINT8(0x02, tx_cin(0xF1, 2));    // MTC Quarter Frame
    TEST_ASSERT_EQUAL_UINT8(0x02, tx_cin(0xF3, 2));    // Song Select
}

// TX 3-byte: the CIN is the status nibble (Note On/Off, CC, PitchBend, ...).
void test_tx_three_byte_status_nibble(void) {
    TEST_ASSERT_EQUAL_UINT8(0x08, tx_cin(0x84, 3));    // Note Off, ch 4
    TEST_ASSERT_EQUAL_UINT8(0x09, tx_cin(0x90, 3));    // Note On, ch 0
    TEST_ASSERT_EQUAL_UINT8(0x0B, tx_cin(0xB1, 3));    // Control Change, ch 1
    TEST_ASSERT_EQUAL_UINT8(0x0E, tx_cin(0xE5, 3));    // Pitch Bend, ch 5
}

// TX 3-byte: Song Position Pointer (0xF2) is the lone 3-byte System Common and
// maps to CIN 0x3 — NOT the 0xF its status nibble would otherwise yield. This
// is the TX mirror of the RX 0x03 -> 3 case (test_rx_three_byte_cins).
void test_tx_three_byte_system_common(void) {
    TEST_ASSERT_EQUAL_UINT8(0x03, tx_cin(0xF2, 3));    // Song Position Pointer
}

// ---------------------------------------------------------------------------
// VBUS presence debouncer (vbus_debounce.hpp) — the state machine behind the PC9
// soft-connect gate. Times are arbitrary ms; VBUS_DEBOUNCE is the stable-for window.
// ---------------------------------------------------------------------------
static constexpr uint32_t VBUS_DEBOUNCE = 50;
static int edge(vbus::Edge e) { return static_cast<int>(e); }

// seed() adopts the starting level and never emits an edge, and a steady line stays quiet.
void test_vbus_seed_is_silent(void) {
    vbus::Debouncer d;
    d.seed(false, 1000);
    TEST_ASSERT_FALSE(d.present());
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(false, 1000, VBUS_DEBOUNCE)));
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(false, 5000, VBUS_DEBOUNCE)));
    TEST_ASSERT_FALSE(d.present());
}

// Plug: the rising edge commits Connected only once VBUS has held for the debounce window.
void test_vbus_plug_commits_after_debounce(void) {
    vbus::Debouncer d;
    d.seed(false, 0);
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(true, 100, VBUS_DEBOUNCE)));  // start settling
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(true, 149, VBUS_DEBOUNCE)));  // 49 ms < 50
    TEST_ASSERT_FALSE(d.present());
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::Connected), edge(d.update(true, 150, VBUS_DEBOUNCE)));  // 50 ms
    TEST_ASSERT_TRUE(d.present());
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(true, 300, VBUS_DEBOUNCE)));  // already up
}

// Unplug: the falling edge commits Disconnected after the same window.
void test_vbus_unplug_commits_after_debounce(void) {
    vbus::Debouncer d;
    d.seed(true, 0);
    TEST_ASSERT_TRUE(d.present());
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(false, 100, VBUS_DEBOUNCE)));
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(false, 149, VBUS_DEBOUNCE)));
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::Disconnected), edge(d.update(false, 150, VBUS_DEBOUNCE)));
    TEST_ASSERT_FALSE(d.present());
}

// A blip shorter than the window never commits — the line returns before it settles.
void test_vbus_short_bounce_rejected(void) {
    vbus::Debouncer d;
    d.seed(false, 0);
    d.update(true, 100, VBUS_DEBOUNCE);                       // candidate high @100
    d.update(false, 120, VBUS_DEBOUNCE);                      // back low after 20 ms
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(false, 900, VBUS_DEBOUNCE)));
    TEST_ASSERT_FALSE(d.present());
}

// A glitch part-way through the window restarts the timer, so the commit waits a full
// window from the *last* transition — not from the first.
void test_vbus_glitch_restarts_window(void) {
    vbus::Debouncer d;
    d.seed(false, 0);
    d.update(true, 100, VBUS_DEBOUNCE);                       // settling from 100
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(true, 130, VBUS_DEBOUNCE)));   // 30 ms in
    d.update(false, 140, VBUS_DEBOUNCE);                      // glitch low resets
    d.update(true, 150, VBUS_DEBOUNCE);                       // settling again from 150
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::None), edge(d.update(true, 199, VBUS_DEBOUNCE)));   // 49 ms < 50
    TEST_ASSERT_EQUAL_INT(edge(vbus::Edge::Connected), edge(d.update(true, 200, VBUS_DEBOUNCE)));  // 50 ms
    TEST_ASSERT_TRUE(d.present());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_rx_single_byte_cins);
    RUN_TEST(test_rx_two_byte_cins);
    RUN_TEST(test_rx_three_byte_cins);
    RUN_TEST(test_rx_ignores_cable_nibble);
    RUN_TEST(test_tx_one_byte);
    RUN_TEST(test_tx_two_byte_channel_voice_split);
    RUN_TEST(test_tx_two_byte_system_common);
    RUN_TEST(test_tx_three_byte_status_nibble);
    RUN_TEST(test_tx_three_byte_system_common);
    RUN_TEST(test_vbus_seed_is_silent);
    RUN_TEST(test_vbus_plug_commits_after_debounce);
    RUN_TEST(test_vbus_unplug_commits_after_debounce);
    RUN_TEST(test_vbus_short_bounce_rejected);
    RUN_TEST(test_vbus_glitch_restarts_window);
    return UNITY_END();
}
