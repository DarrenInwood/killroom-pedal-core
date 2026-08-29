// Host-native unit tests for the routing translation (include/pedal_core/midi_routing.hpp).
//
// The twelve-byte block on the wire and the pedal's MIDI configuration are the same
// settings in two vocabularies, and this is the one place that knows both. The properties
// worth pinning are the ones a product would otherwise re-derive from raw byte offsets:
// all twelve fields arrive somewhere named, the two spellings of "follow the receive
// channel" reconcile, and a value outside its range is clamped rather than believed.

#include <unity.h>
#include <cstdint>

#include <pedal_core/midi_routing.hpp>

using pedal_core::routing::Settings;
using pedal_core::routing::from_wire;
using pedal_core::routing::to_wire;
namespace wr = pedal_core::wire::midi_routing;
using Block = wr::RoutingBlock;

void setUp(void) {}
void tearDown(void) {}

// A block with nothing left at its default, so a field that fails to arrive shows up as
// itself rather than as a coincidence.
static Block sample_block(void) {
    Block b;
    b.rx_channel    = 5u;
    b.omni          = true;
    b.tx_channel    = 11u;
    b.out           = wr::out_mode::THRU;
    b.pc_offset     = 1u;
    b.clock_out     = true;
    b.clock_thru    = false;
    b.usb_jack_route = wr::usb_jack::BOTH;
    b.rx_pc         = false;
    b.rx_sysex      = false;
    b.tx_params     = false;
    b.tx            = wr::tx_state::PC_BYPASS;
    return b;
}

// Every one of the twelve bytes arrives somewhere named — nine on the handler's config
// and three on the settings around it. Nothing is dropped for the product to dig back
// out of the raw offsets.
void test_all_twelve_fields_arrive_named(void) {
    const Settings s = from_wire(sample_block());

    TEST_ASSERT_EQUAL_UINT8(5u, s.config.channel);
    TEST_ASSERT_TRUE(s.config.omni);
    TEST_ASSERT_EQUAL_UINT8(11u, s.config.tx_channel);
    TEST_ASSERT_EQUAL_INT((int)midi_handler::OutMode::Thru, (int)s.config.out_mode);
    TEST_ASSERT_EQUAL_INT((int)midi_handler::UsbJackRoute::Both, (int)s.config.usb_jack);
    TEST_ASSERT_FALSE(s.config.clock_thru);
    TEST_ASSERT_FALSE(s.config.rx_pc);
    TEST_ASSERT_FALSE(s.config.rx_sysex);
    TEST_ASSERT_FALSE(s.config.tx_params);

    // The three that belong elsewhere: the base's, the generator's, the product's.
    TEST_ASSERT_EQUAL_UINT8(1u, s.pc_offset);
    TEST_ASSERT_TRUE(s.clock_out);
    TEST_ASSERT_EQUAL_UINT8(wr::tx_state::PC_BYPASS, s.tx_state);
}

// A host writes back exactly what it read, so the trip out and back has to be lossless.
void test_the_block_round_trips_through_the_settings(void) {
    const Block sent = sample_block();
    const Block got  = to_wire(from_wire(sent));

    TEST_ASSERT_EQUAL_UINT8(sent.rx_channel, got.rx_channel);
    TEST_ASSERT_EQUAL_INT(sent.omni, got.omni);
    TEST_ASSERT_EQUAL_UINT8(sent.tx_channel, got.tx_channel);
    TEST_ASSERT_EQUAL_UINT8(sent.out, got.out);
    TEST_ASSERT_EQUAL_UINT8(sent.pc_offset, got.pc_offset);
    TEST_ASSERT_EQUAL_INT(sent.clock_out, got.clock_out);
    TEST_ASSERT_EQUAL_INT(sent.clock_thru, got.clock_thru);
    TEST_ASSERT_EQUAL_UINT8(sent.usb_jack_route, got.usb_jack_route);
    TEST_ASSERT_EQUAL_INT(sent.rx_pc, got.rx_pc);
    TEST_ASSERT_EQUAL_INT(sent.rx_sysex, got.rx_sysex);
    TEST_ASSERT_EQUAL_INT(sent.tx_params, got.tx_params);
    TEST_ASSERT_EQUAL_UINT8(sent.tx, got.tx);
}

// "Follow the receive channel" is 0x7F on the wire, because 0xFF is not a legal SysEx
// data byte, and 0xFF in the handler. Both are load-bearing; this is where they meet.
void test_the_follow_sentinel_changes_spelling_in_both_directions(void) {
    Block b;
    b.tx_channel = wr::TX_CHANNEL_FOLLOW_RX;              // 0x7F
    TEST_ASSERT_EQUAL_UINT8(midi_handler::TX_CHANNEL_FOLLOW_RX, from_wire(b).config.tx_channel);

    Settings s;
    s.config.tx_channel = midi_handler::TX_CHANNEL_FOLLOW_RX;   // 0xFF
    TEST_ASSERT_EQUAL_UINT8(wr::TX_CHANNEL_FOLLOW_RX, to_wire(s).tx_channel);

    // The two spellings really are different bytes, so neither direction is a no-op.
    TEST_ASSERT_NOT_EQUAL(wr::TX_CHANNEL_FOLLOW_RX, midi_handler::TX_CHANNEL_FOLLOW_RX);
}

// An explicit transmit channel is carried through rather than collapsed into the
// sentinel: a pedal listening in Omni still speaks on the one channel it was given.
void test_an_explicit_transmit_channel_survives(void) {
    Block b;
    b.omni       = true;
    b.tx_channel = 3u;
    const Settings s = from_wire(b);
    TEST_ASSERT_TRUE(s.config.omni);
    TEST_ASSERT_EQUAL_UINT8(3u, s.config.tx_channel);
    TEST_ASSERT_EQUAL_UINT8(3u, to_wire(s).tx_channel);
}

// A block with nothing set is the behaviour a pedal has with no block stored, so a
// default value has to land on a default configuration rather than a silent jack.
void test_a_default_block_is_a_default_configuration(void) {
    const Settings s = from_wire(Block{});
    const midi_handler::Config d{};

    TEST_ASSERT_EQUAL_UINT8(d.channel, s.config.channel);
    TEST_ASSERT_EQUAL_INT(d.omni, s.config.omni);
    TEST_ASSERT_EQUAL_UINT8(d.tx_channel, s.config.tx_channel);
    TEST_ASSERT_EQUAL_INT((int)d.out_mode, (int)s.config.out_mode);
    TEST_ASSERT_EQUAL_INT((int)d.usb_jack, (int)s.config.usb_jack);
    TEST_ASSERT_EQUAL_INT(d.clock_thru, s.config.clock_thru);
    TEST_ASSERT_EQUAL_INT(d.rx_pc, s.config.rx_pc);
    TEST_ASSERT_EQUAL_INT(d.rx_sysex, s.config.rx_sysex);
    TEST_ASSERT_EQUAL_INT(d.tx_params, s.config.tx_params);
    TEST_ASSERT_EQUAL_UINT8(0u, s.pc_offset);
    TEST_ASSERT_FALSE(s.clock_out);
    TEST_ASSERT_EQUAL_UINT8(wr::tx_state::OFF, s.tx_state);
}

// The wire contract says a range is the pedal's to enforce, so a host that sends a fifth
// out-mode gets the default rather than an undefined one.
void test_an_out_of_range_mode_clamps_to_its_default(void) {
    Block b;
    b.out           = wr::out_mode::COUNT;        // one past the last
    b.usb_jack_route = 99u;
    b.tx            = wr::tx_state::COUNT;
    const Settings s = from_wire(b);

    TEST_ASSERT_EQUAL_INT((int)midi_handler::OutMode::Merge, (int)s.config.out_mode);
    TEST_ASSERT_EQUAL_INT((int)midi_handler::UsbJackRoute::Off, (int)s.config.usb_jack);
    TEST_ASSERT_EQUAL_UINT8(wr::tx_state::OFF, s.tx_state);
}

// A receive channel past the sixteen that exist is read as the first, not as itself.
void test_an_out_of_range_receive_channel_clamps(void) {
    Block b;
    b.rx_channel = 16u;                           // omni is its own flag, not a channel
    TEST_ASSERT_EQUAL_UINT8(0u, from_wire(b).config.channel);
}

// A transmit channel that is neither a channel nor the sentinel is read as "follow",
// which is the safe reading of a byte we cannot place: the pedal is never left silent.
void test_an_unplaceable_transmit_channel_reads_as_follow(void) {
    Block b;
    b.tx_channel = 42u;
    TEST_ASSERT_EQUAL_UINT8(midi_handler::TX_CHANNEL_FOLLOW_RX, from_wire(b).config.tx_channel);
}

// Every value the wire can carry translates to something and comes back unchanged, so no
// legal byte falls through the mapping.
void test_every_legal_mode_survives_the_trip(void) {
    for (uint8_t m = 0; m < wr::out_mode::COUNT; ++m) {
        Block b; b.out = m;
        TEST_ASSERT_EQUAL_UINT8(m, to_wire(from_wire(b)).out);
    }
    for (uint8_t r = 0; r < wr::usb_jack::COUNT; ++r) {
        Block b; b.usb_jack_route = r;
        TEST_ASSERT_EQUAL_UINT8(r, to_wire(from_wire(b)).usb_jack_route);
    }
    for (uint8_t t = 0; t < wr::tx_state::COUNT; ++t) {
        Block b; b.tx = t;
        TEST_ASSERT_EQUAL_UINT8(t, to_wire(from_wire(b)).tx);
    }
    for (uint8_t c = 0; c < 16u; ++c) {
        Block b; b.rx_channel = c; b.tx_channel = c;
        const Block got = to_wire(from_wire(b));
        TEST_ASSERT_EQUAL_UINT8(c, got.rx_channel);
        TEST_ASSERT_EQUAL_UINT8(c, got.tx_channel);
    }
}

// The offset spans the full 7-bit range a Program Change can address.
void test_the_program_change_offset_spans_its_range(void) {
    Block b;
    b.pc_offset = 127u;
    const Settings s = from_wire(b);
    TEST_ASSERT_EQUAL_UINT8(127u, s.pc_offset);
    TEST_ASSERT_EQUAL_UINT8(127u, to_wire(s).pc_offset);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_twelve_fields_arrive_named);
    RUN_TEST(test_the_block_round_trips_through_the_settings);
    RUN_TEST(test_the_follow_sentinel_changes_spelling_in_both_directions);
    RUN_TEST(test_an_explicit_transmit_channel_survives);
    RUN_TEST(test_a_default_block_is_a_default_configuration);
    RUN_TEST(test_an_out_of_range_mode_clamps_to_its_default);
    RUN_TEST(test_an_out_of_range_receive_channel_clamps);
    RUN_TEST(test_an_unplaceable_transmit_channel_reads_as_follow);
    RUN_TEST(test_every_legal_mode_survives_the_trip);
    RUN_TEST(test_the_program_change_offset_spans_its_range);
    return UNITY_END();
}
