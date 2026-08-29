// Host-native unit tests for the MIDI Out queue (include/pedal_core/midi_out_queue.hpp).
//
// MIDI In and MIDI Out share 31.25 kbaud, so a pedal that both forwards an inbound stream and
// speaks for itself has more to send than the wire can carry, and USB In runs orders of
// magnitude faster than the jack. Something has to be lost; this is what decides what.
//
// The queue holds whole messages rather than bytes, because coalescing needs identity and
// identity needs message boundaries: a byte ring cannot find the earlier value of CC 74 on
// channel 3 in order to replace it. What leaves the jack is the current value of a controller
// rather than every intermediate one — except where a Control Change is not continuous state,
// which is most of what these tests are about.
//
// The whole module is driven directly. It touches no hardware and reads no clock: it holds
// messages and decides which survive.

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <pedal_core/midi_out_queue.hpp>

using pedal_core::MidiOutQueue;
using pedal_core::MidiMessage;

static MidiOutQueue* q = nullptr;

void setUp(void)    { q = new MidiOutQueue(); }
void tearDown(void) { delete q; q = nullptr; }

// --- helpers ----------------------------------------------------------------

static MidiMessage msg3(uint8_t status, uint8_t d0, uint8_t d1) {
    MidiMessage m; m.status = status; m.data[0] = d0; m.data[1] = d1; m.len = 3u; return m;
}
static MidiMessage msg2(uint8_t status, uint8_t d0) {
    MidiMessage m; m.status = status; m.data[0] = d0; m.len = 2u; return m;
}
static MidiMessage cc(uint8_t channel, uint8_t controller, uint8_t value) {
    return msg3((uint8_t)(0xB0u | channel), controller, value);
}

// Everything the queue holds, in order, as "status:d0:d1".
static std::vector<std::string> drained() {
    std::vector<std::string> out;
    MidiMessage m;
    while (q->pop(m)) {
        char buf[24];
        if (m.len == 3u)      snprintf(buf, sizeof(buf), "%02X:%02X:%02X", m.status, m.data[0], m.data[1]);
        else if (m.len == 2u) snprintf(buf, sizeof(buf), "%02X:%02X", m.status, m.data[0]);
        else                  snprintf(buf, sizeof(buf), "%02X", m.status);
        out.push_back(buf);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Order, and the shape of a message
// ---------------------------------------------------------------------------

void test_an_empty_queue_yields_nothing(void) {
    MidiMessage m;
    TEST_ASSERT_FALSE(q->pop(m));
    TEST_ASSERT_TRUE(q->empty());
    TEST_ASSERT_EQUAL_UINT16(0u, q->size());
}

// Messages that cannot coalesce leave in the order they arrived.
void test_messages_leave_in_the_order_they_arrived(void) {
    q->push(msg3(0x90u, 0x40u, 0x7Fu), MidiOutQueue::Origin::Forwarded);       // note on
    q->push(msg3(0x80u, 0x40u, 0x00u), MidiOutQueue::Origin::Forwarded);       // note off
    q->push(msg2(0xC0u, 0x05u), MidiOutQueue::Origin::Forwarded);              // program change
    TEST_ASSERT_EQUAL_UINT16(3u, q->size());

    const std::vector<std::string> got = drained();
    TEST_ASSERT_EQUAL_STRING("90:40:7F", got[0].c_str());
    TEST_ASSERT_EQUAL_STRING("80:40:00", got[1].c_str());
    TEST_ASSERT_EQUAL_STRING("C0:05",    got[2].c_str());
    TEST_ASSERT_TRUE(q->empty());
}

// ---------------------------------------------------------------------------
// Coalescing: the same identity carries the current value, not every step
// ---------------------------------------------------------------------------

// A controller sweep leaves as its final value, in the position the sweep started.
void test_a_controller_sweep_coalesces_to_its_current_value(void) {
    q->push(msg3(0x90u, 0x40u, 0x7Fu), MidiOutQueue::Origin::Forwarded);       // a note, ahead of the sweep
    q->push(cc(0u, 74u, 10u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(0u, 74u, 20u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(0u, 74u, 30u), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(2u, q->size());

    const std::vector<std::string> got = drained();
    TEST_ASSERT_EQUAL_STRING("90:40:7F", got[0].c_str());
    TEST_ASSERT_EQUAL_STRING("B0:4A:1E", got[1].c_str());   // CC 74 = 30, where it started
}

// Identity is the channel and the controller together.
void test_coalescing_is_per_channel_and_per_controller(void) {
    q->push(cc(0u, 74u, 10u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(1u, 74u, 20u), MidiOutQueue::Origin::Forwarded);                // same controller, other channel
    q->push(cc(0u, 75u, 30u), MidiOutQueue::Origin::Forwarded);                // same channel, other controller
    q->push(cc(0u, 74u, 40u), MidiOutQueue::Origin::Forwarded);                // coalesces with the first
    TEST_ASSERT_EQUAL_UINT16(3u, q->size());

    const std::vector<std::string> got = drained();
    TEST_ASSERT_EQUAL_STRING("B0:4A:28", got[0].c_str());   // ch 0 CC 74 = 40
    TEST_ASSERT_EQUAL_STRING("B1:4A:14", got[1].c_str());   // ch 1 CC 74 = 20
    TEST_ASSERT_EQUAL_STRING("B0:4B:1E", got[2].c_str());   // ch 0 CC 75 = 30
}

// Pitch Bend and Channel Pressure are continuous state too, identified by channel alone.
void test_pitch_bend_and_channel_pressure_coalesce_by_channel(void) {
    q->push(msg3(0xE0u, 0x00u, 0x40u), MidiOutQueue::Origin::Forwarded);       // pitch bend, ch 0
    q->push(msg3(0xE0u, 0x7Fu, 0x50u), MidiOutQueue::Origin::Forwarded);
    q->push(msg2(0xD0u, 0x20u), MidiOutQueue::Origin::Forwarded);              // channel pressure, ch 0
    q->push(msg2(0xD0u, 0x30u), MidiOutQueue::Origin::Forwarded);
    q->push(msg3(0xE1u, 0x00u, 0x60u), MidiOutQueue::Origin::Forwarded);       // pitch bend, ch 1: its own identity
    TEST_ASSERT_EQUAL_UINT16(3u, q->size());

    const std::vector<std::string> got = drained();
    TEST_ASSERT_EQUAL_STRING("E0:7F:50", got[0].c_str());
    TEST_ASSERT_EQUAL_STRING("D0:30",    got[1].c_str());
    TEST_ASSERT_EQUAL_STRING("E1:00:60", got[2].c_str());
}

// Notes are events, not state: two note-ons for the same pitch are two notes.
void test_notes_never_coalesce(void) {
    q->push(msg3(0x90u, 0x40u, 0x7Fu), MidiOutQueue::Origin::Forwarded);
    q->push(msg3(0x90u, 0x40u, 0x64u), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(2u, q->size());
}

// ---------------------------------------------------------------------------
// The exclusions: a Control Change that is not continuous state
// ---------------------------------------------------------------------------

// CC 6, 38 and 96-101 are the RPN/NRPN transaction. A data-entry value against a stale
// parameter select is corrupt, and this family's 14-bit NRPN scale is live wire protocol.
void test_the_nrpn_transaction_stays_in_order(void) {
    const uint8_t transaction[] = { 99u, 98u, 6u, 38u, 96u, 97u, 100u, 101u };
    for (uint8_t c : transaction) q->push(cc(0u, c, 1u), MidiOutQueue::Origin::Forwarded);
    for (uint8_t c : transaction) q->push(cc(0u, c, 2u), MidiOutQueue::Origin::Forwarded);

    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(sizeof(transaction) * 2u), q->size(),
                                     "an NRPN controller coalesced");
}

// CC 0 and 32 are Bank Select, a transaction with the Program Change that follows.
void test_bank_select_stays_in_order(void) {
    q->push(cc(0u, 0u, 1u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(0u, 32u, 2u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(0u, 0u, 3u), MidiOutQueue::Origin::Forwarded);
    q->push(cc(0u, 32u, 4u), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(4u, q->size());
}

// CC 120-127 are Channel Mode messages: events, not state.
void test_channel_mode_messages_stay_in_order(void) {
    for (uint8_t c = 120u; c <= 127u; ++c) { q->push(cc(0u, c, 0u), MidiOutQueue::Origin::Forwarded); q->push(cc(0u, c, 0u), MidiOutQueue::Origin::Forwarded); }
    TEST_ASSERT_EQUAL_UINT16(16u, q->size());
}

// Every controller outside those ranges is continuous state and does coalesce.
void test_every_other_controller_coalesces(void) {
    const uint8_t excluded[] = { 0u, 32u, 6u, 38u, 96u, 97u, 98u, 99u, 100u, 101u,
                                 120u, 121u, 122u, 123u, 124u, 125u, 126u, 127u };
    for (uint8_t c = 0u; c < 128u; ++c) {
        bool is_excluded = false;
        for (uint8_t e : excluded) if (e == c) is_excluded = true;
        if (is_excluded) continue;

        MidiOutQueue one;
        one.push(cc(0u, c, 10u), MidiOutQueue::Origin::Forwarded);
        one.push(cc(0u, c, 20u), MidiOutQueue::Origin::Forwarded);
        char why[48];
        snprintf(why, sizeof(why), "CC %u did not coalesce", (unsigned)c);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u, one.size(), why);
    }
}

// MIDI Time Code quarter-frames each carry a different nibble index, so merging them
// destroys the timecode.
void test_time_code_quarter_frames_never_merge(void) {
    for (uint8_t nibble = 0u; nibble < 8u; ++nibble)
        q->push(msg2(0xF1u, (uint8_t)(nibble << 4)), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(8u, q->size());

    const std::vector<std::string> got = drained();
    TEST_ASSERT_EQUAL_STRING("F1:00", got[0].c_str());
    TEST_ASSERT_EQUAL_STRING("F1:70", got[7].c_str());
}


// ---------------------------------------------------------------------------
// A full queue: what is admitted, and what it costs
//
// Something has to be lost when there is more to send than the wire can carry. A dropped
// Note On is a missing note; a dropped Note Off is a stuck one, and the two are not worth
// the same. Own traffic is low-rate by construction, so it cannot itself cause the overflow
// and a pedal unable to speak on its own jack fails worse than one dropping a forwarded
// controller sweep.
// ---------------------------------------------------------------------------

// Fill with messages that cannot coalesce, each identifiable by its data byte.
static void fill_with_non_coalescable(MidiOutQueue& queue, MidiOutQueue::Origin origin) {
    for (uint16_t i = 0u; i < MidiOutQueue::DEPTH; ++i)
        queue.push(msg2(0xC0u, (uint8_t)(i & 0x7Fu)), origin);   // program change
}

// An ordinary forwarded message arriving at a full queue is refused, and takes nothing with
// it: the backlog already queued is worth more than the newest controller step.
void test_a_full_queue_refuses_an_ordinary_forwarded_message(void) {
    fill_with_non_coalescable(*q, MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(MidiOutQueue::DEPTH, q->size());

    TEST_ASSERT_FALSE(q->push(cc(0u, 74u, 99u), MidiOutQueue::Origin::Forwarded));
    TEST_ASSERT_EQUAL_UINT16(MidiOutQueue::DEPTH, q->size());

    MidiMessage first;
    TEST_ASSERT_TRUE(q->pop(first));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xC0u, first.status, "the head of the queue was disturbed");
}

// A Note Off is never dropped. It evicts the oldest thing that can be said again -- a
// controller value the sender will send another of -- rather than a note nobody repeats.
void test_a_note_off_evicts_the_oldest_coalescable_entry(void) {
    q->push(cc(0u, 74u, 10u), MidiOutQueue::Origin::Forwarded);    // the oldest coalescable
    q->push(cc(0u, 75u, 20u), MidiOutQueue::Origin::Forwarded);
    for (uint16_t i = 2u; i < MidiOutQueue::DEPTH; ++i)
        q->push(msg2(0xC0u, (uint8_t)(i & 0x7Fu)), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(MidiOutQueue::DEPTH, q->size());

    TEST_ASSERT_TRUE(q->push(msg3(0x80u, 0x40u, 0x00u), MidiOutQueue::Origin::Forwarded));
    TEST_ASSERT_EQUAL_UINT16(MidiOutQueue::DEPTH, q->size());

    // CC 74 is gone; CC 75 is not, and the note off is at the back.
    bool saw_74 = false, saw_75 = false;
    MidiMessage m, last;
    while (q->pop(m)) {
        if ((m.status & 0xF0u) == 0xB0u && m.data[0] == 74u) saw_74 = true;
        if ((m.status & 0xF0u) == 0xB0u && m.data[0] == 75u) saw_75 = true;
        last = m;
    }
    TEST_ASSERT_FALSE_MESSAGE(saw_74, "the oldest coalescable entry was not the one evicted");
    TEST_ASSERT_TRUE_MESSAGE(saw_75, "a coalescable entry that was not the oldest was evicted");
    TEST_ASSERT_EQUAL_UINT8(0x80u, last.status);
}

// With nothing coalescable to give up, a Note Off costs the oldest message instead.
void test_a_note_off_falls_back_to_evicting_the_oldest(void) {
    fill_with_non_coalescable(*q, MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_TRUE(q->push(msg3(0x80u, 0x40u, 0x00u), MidiOutQueue::Origin::Forwarded));

    MidiMessage first;
    q->pop(first);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, first.data[0],
                                    "the oldest message was not the one evicted");
}

// All-notes-off and all-sound-off are the same promise as a Note Off: without them a note
// hangs, so they are admitted on the same terms.
void test_all_sound_off_and_all_notes_off_are_protected(void) {
    for (uint8_t controller : { 120u, 123u }) {
        MidiOutQueue one;
        fill_with_non_coalescable(one, MidiOutQueue::Origin::Forwarded);
        char why[48];
        snprintf(why, sizeof(why), "CC %u was dropped at a full queue", (unsigned)controller);
        TEST_ASSERT_TRUE_MESSAGE(one.push(cc(0u, controller, 0u),
                                          MidiOutQueue::Origin::Forwarded), why);
    }
}

// A note on with velocity zero is the other spelling of a note off, and a keyboard chooses
// which to send. Protecting one and not the other would hang a note on half the controllers
// in the world.
void test_a_note_on_with_velocity_zero_is_protected_too(void) {
    fill_with_non_coalescable(*q, MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_TRUE(q->push(msg3(0x90u, 0x40u, 0x00u), MidiOutQueue::Origin::Forwarded));

    // And a real note on is not: it is a missing note rather than a stuck one.
    MidiOutQueue other;
    fill_with_non_coalescable(other, MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_FALSE(other.push(msg3(0x90u, 0x40u, 0x7Fu), MidiOutQueue::Origin::Forwarded));
}

// The pedal's own traffic is admitted under pressure, and pays for it with a forwarded
// entry rather than one of its own.
void test_own_traffic_outranks_forwarded_traffic(void) {
    q->push(cc(0u, 74u, 10u), MidiOutQueue::Origin::Own);          // own, coalescable, oldest
    q->push(cc(0u, 75u, 20u), MidiOutQueue::Origin::Forwarded);    // forwarded, coalescable
    for (uint16_t i = 2u; i < MidiOutQueue::DEPTH; ++i)
        q->push(msg2(0xC0u, (uint8_t)(i & 0x7Fu)), MidiOutQueue::Origin::Forwarded);

    TEST_ASSERT_TRUE(q->push(msg2(0xC0u, 0x7Fu), MidiOutQueue::Origin::Own));

    bool saw_own_74 = false, saw_forwarded_75 = false;
    MidiMessage m;
    while (q->pop(m)) {
        if ((m.status & 0xF0u) == 0xB0u && m.data[0] == 74u) saw_own_74 = true;
        if ((m.status & 0xF0u) == 0xB0u && m.data[0] == 75u) saw_forwarded_75 = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_own_74, "the pedal evicted its own message over a forwarded one");
    TEST_ASSERT_FALSE_MESSAGE(saw_forwarded_75, "the forwarded coalescable entry survived");
}

// With nothing forwarded left to give up, own traffic evicts its own oldest rather than
// refusing itself.
void test_own_traffic_evicts_its_own_when_nothing_forwarded_remains(void) {
    fill_with_non_coalescable(*q, MidiOutQueue::Origin::Own);
    TEST_ASSERT_TRUE(q->push(msg2(0xC0u, 0x7Fu), MidiOutQueue::Origin::Own));
    TEST_ASSERT_EQUAL_UINT16(MidiOutQueue::DEPTH, q->size());
}

// Coalescing is what keeps a sweep from ever reaching the full-queue rules: however long it
// runs it occupies one entry.
void test_a_sweep_never_fills_the_queue(void) {
    for (uint16_t i = 0u; i < 10000u; ++i)
        q->push(cc(0u, 74u, (uint8_t)(i & 0x7Fu)), MidiOutQueue::Origin::Forwarded);
    TEST_ASSERT_EQUAL_UINT16(1u, q->size());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_queue_yields_nothing);
    RUN_TEST(test_messages_leave_in_the_order_they_arrived);
    RUN_TEST(test_a_controller_sweep_coalesces_to_its_current_value);
    RUN_TEST(test_coalescing_is_per_channel_and_per_controller);
    RUN_TEST(test_pitch_bend_and_channel_pressure_coalesce_by_channel);
    RUN_TEST(test_notes_never_coalesce);
    RUN_TEST(test_the_nrpn_transaction_stays_in_order);
    RUN_TEST(test_bank_select_stays_in_order);
    RUN_TEST(test_channel_mode_messages_stay_in_order);
    RUN_TEST(test_every_other_controller_coalesces);
    RUN_TEST(test_time_code_quarter_frames_never_merge);
    RUN_TEST(test_a_full_queue_refuses_an_ordinary_forwarded_message);
    RUN_TEST(test_a_note_off_evicts_the_oldest_coalescable_entry);
    RUN_TEST(test_a_note_off_falls_back_to_evicting_the_oldest);
    RUN_TEST(test_all_sound_off_and_all_notes_off_are_protected);
    RUN_TEST(test_a_note_on_with_velocity_zero_is_protected_too);
    RUN_TEST(test_own_traffic_outranks_forwarded_traffic);
    RUN_TEST(test_own_traffic_evicts_its_own_when_nothing_forwarded_remains);
    RUN_TEST(test_a_sweep_never_fills_the_queue);
    return UNITY_END();
}
