// Host-native unit tests for the DIN Out router (include/pedal_core/din_router.hpp).
//
// The router arbitrates the DIN Out jack between the three things that want it: the
// inbound stream being echoed, the other transport being cross-routed, and the pedal's
// own traffic. Only System Real-Time bytes may appear inside another message, so
// everything else has to leave the jack whole -- a message contending with a frame
// already streaming waits in a queue rather than splicing into it.
//
// The whole module is driven here directly. It reads no clock (every entry point that
// needs one is given it) and it writes to exactly one place, so the only thing this suite
// stands up is a recording uart::write. There is no parser upstream and no byte stream to
// fake, which is the point of giving the router an interface of its own.

#include <unity.h>
#include <cstdint>
#include <vector>

// The one contract the router reaches hardware through.
namespace uart {
    static std::vector<uint8_t> g_wire;   // every byte that reached the jack
    void write(uint8_t b) { g_wire.push_back(b); }
}

#include <pedal_core/din_router.hpp>

using pedal_core::DinRouter;
using Src = DinRouter::Src;
using midi_handler::Config;
using midi_handler::OutMode;
using midi_handler::UsbDinRoute;

static DinRouter* g_r = nullptr;

void setUp(void) {
    uart::g_wire.clear();
    g_r = new DinRouter();
}
void tearDown(void) { delete g_r; g_r = nullptr; }

// --- helpers ----------------------------------------------------------------

static void configure(OutMode out, UsbDinRoute cross = UsbDinRoute::Off, bool clock_thru = true) {
    Config c;
    c.out_mode   = out;
    c.usb_din    = cross;
    c.clock_thru = clock_thru;
    g_r->set_config(c);
}

static void send(Src src, std::vector<uint8_t> msg) {
    g_r->message(src, msg.data(), (uint16_t)msg.size());
}

static bool wire_is(std::vector<uint8_t> expected) {
    return uart::g_wire == expected;
}

// ---------------------------------------------------------------------------
// Carries-policy, as a table
//
// The rules that surprise people live here, and a truth table is the shape they read in.
// Driven through a byte stream they would be unreadable, which is why they are part of
// the router's interface -- but every one of them is also covered below through the
// entry points that consult them, so this is not coverage of rules nobody calls.
// ---------------------------------------------------------------------------

// An inbound stream is echoed on Merge and Thru, and not on Out or Off.
void test_an_inbound_stream_is_echoed_on_merge_and_thru(void) {
    const OutMode carries[]     = { OutMode::Merge, OutMode::Thru };
    const OutMode carries_not[] = { OutMode::Out,   OutMode::Off  };
    for (OutMode m : carries)     { configure(m); TEST_ASSERT_TRUE(g_r->carries(Src::Uart)); }
    for (OutMode m : carries_not) { configure(m); TEST_ASSERT_FALSE(g_r->carries(Src::Uart)); }
}

// The pedal's own traffic leaves on Merge and Out, and not on Thru or Off.
void test_the_pedals_own_traffic_leaves_on_merge_and_out(void) {
    const OutMode carries[]     = { OutMode::Merge, OutMode::Out  };
    const OutMode carries_not[] = { OutMode::Thru,  OutMode::Off  };
    for (OutMode m : carries)     { configure(m); TEST_ASSERT_TRUE(g_r->carries(Src::Self)); }
    for (OutMode m : carries_not) { configure(m); TEST_ASSERT_FALSE(g_r->carries(Src::Self)); }
}

// USB reaches the DIN jack only where the echo is on AND the cross-route points that way.
void test_usb_reaches_din_only_when_both_switches_agree(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    TEST_ASSERT_TRUE(g_r->carries(Src::Usb));
    configure(OutMode::Merge, UsbDinRoute::Both);
    TEST_ASSERT_TRUE(g_r->carries(Src::Usb));

    configure(OutMode::Merge, UsbDinRoute::Off);
    TEST_ASSERT_FALSE(g_r->carries(Src::Usb));      // echo on, cross-route off
    configure(OutMode::Merge, UsbDinRoute::DinToUsb);
    TEST_ASSERT_FALSE(g_r->carries(Src::Usb));      // pointing the other way
    configure(OutMode::Out, UsbDinRoute::UsbToDin);
    TEST_ASSERT_FALSE(g_r->carries(Src::Usb));      // cross-route on, echo off
}

// DIN reaches USB only on the cross-routes that point that way, whatever the jack does.
void test_din_reaches_usb_only_on_the_cross_routes_that_point_that_way(void) {
    configure(OutMode::Off, UsbDinRoute::DinToUsb);
    TEST_ASSERT_TRUE(g_r->usb_carries(Src::Uart));  // the DIN jack being silent is irrelevant
    configure(OutMode::Merge, UsbDinRoute::Both);
    TEST_ASSERT_TRUE(g_r->usb_carries(Src::Uart));

    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    TEST_ASSERT_FALSE(g_r->usb_carries(Src::Uart));
    configure(OutMode::Merge, UsbDinRoute::Both);
    TEST_ASSERT_FALSE(g_r->usb_carries(Src::Usb));  // USB does not echo to itself
    TEST_ASSERT_FALSE(g_r->usb_carries(Src::Self));
}

// Active Sensing describes one link, not the stream on it. Forwarding it makes a
// downstream device expect a heartbeat this pedal is not promising to keep, so it is
// dropped on every setting.
void test_active_sensing_is_dropped_on_every_setting(void) {
    for (OutMode m : { OutMode::Merge, OutMode::Thru, OutMode::Out, OutMode::Off }) {
        configure(m);
        TEST_ASSERT_FALSE(g_r->carries_realtime(Src::Uart, 0xFEu));
    }
}

// The clock family rides its own switch, so a pedal can be tempo master for the chain
// below it while still listening to a clock above.
void test_the_clock_family_rides_the_clock_thru_switch(void) {
    for (uint8_t status : { 0xF8u, 0xFAu, 0xFBu, 0xFCu }) {
        configure(OutMode::Merge, UsbDinRoute::Off, /*clock_thru=*/true);
        TEST_ASSERT_TRUE(g_r->carries_realtime(Src::Uart, status));
        configure(OutMode::Merge, UsbDinRoute::Off, /*clock_thru=*/false);
        TEST_ASSERT_FALSE(g_r->carries_realtime(Src::Uart, status));
    }
}

// While the pedal generates its own clock the inbound one is dropped whatever that
// switch says: two clocks on one wire read as neither.
void test_a_generated_clock_suppresses_the_inbound_one(void) {
    configure(OutMode::Merge, UsbDinRoute::Off, /*clock_thru=*/true);
    g_r->set_generating_clock(true);
    for (uint8_t status : { 0xF8u, 0xFAu, 0xFBu, 0xFCu })
        TEST_ASSERT_FALSE(g_r->carries_realtime(Src::Uart, status));

    // And the rest of System Real-Time is unaffected by it.
    TEST_ASSERT_TRUE(g_r->carries_realtime(Src::Uart, 0xFFu));
}

// System Reset is a panic message; it travels with the echo.
void test_system_reset_travels_with_the_echo(void) {
    configure(OutMode::Merge); TEST_ASSERT_TRUE(g_r->carries_realtime(Src::Uart, 0xFFu));
    configure(OutMode::Thru);  TEST_ASSERT_TRUE(g_r->carries_realtime(Src::Uart, 0xFFu));
    configure(OutMode::Out);   TEST_ASSERT_FALSE(g_r->carries_realtime(Src::Uart, 0xFFu));
    configure(OutMode::Off);   TEST_ASSERT_FALSE(g_r->carries_realtime(Src::Uart, 0xFFu));
}

// ---------------------------------------------------------------------------
// The same rules, through the entry points that consult them
// ---------------------------------------------------------------------------

void test_a_message_the_policy_refuses_never_reaches_the_jack(void) {
    configure(OutMode::Out);                        // no inbound echo
    send(Src::Uart, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(uart::g_wire.empty());

    configure(OutMode::Merge);
    send(Src::Uart, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(wire_is({0x90, 0x40, 0x7F}));
}

// The cross-route decides whether USB traffic reaches the jack, driven through the entry
// point rather than asked of the predicate: it is the subtlest column of the table, and a
// rule covered only where it is stated is a rule nothing proves is consulted.
void test_usb_traffic_reaches_the_jack_only_where_the_cross_route_says(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    send(Src::Usb, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(wire_is({0x90, 0x40, 0x7F}));

    uart::g_wire.clear();
    configure(OutMode::Merge, UsbDinRoute::Both);
    send(Src::Usb, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(wire_is({0x90, 0x40, 0x7F}));

    uart::g_wire.clear();
    configure(OutMode::Merge, UsbDinRoute::Off);       // echo on, cross-route off
    send(Src::Usb, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(uart::g_wire.empty());

    configure(OutMode::Merge, UsbDinRoute::DinToUsb);  // pointing the other way
    send(Src::Usb, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(uart::g_wire.empty());

    configure(OutMode::Out, UsbDinRoute::UsbToDin);    // cross-route on, echo off
    send(Src::Usb, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(uart::g_wire.empty());
}

// The same for a real-time byte arriving on USB.
void test_a_usb_realtime_byte_obeys_the_cross_route(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    g_r->realtime(Src::Usb, 0xFFu);
    TEST_ASSERT_TRUE(wire_is({0xFF}));

    uart::g_wire.clear();
    configure(OutMode::Merge, UsbDinRoute::Off);
    g_r->realtime(Src::Usb, 0xFFu);
    TEST_ASSERT_TRUE(uart::g_wire.empty());
}

// Two clocks on one wire read as neither, so while the pedal generates one the inbound
// clock is dropped -- driven through realtime(), not merely asked of the predicate.
void test_an_inbound_clock_is_dropped_while_the_pedal_generates(void) {
    configure(OutMode::Merge, UsbDinRoute::Off, /*clock_thru=*/true);
    g_r->realtime(Src::Uart, 0xF8u);
    TEST_ASSERT_TRUE(wire_is({0xF8}));                 // forwarded while not generating

    uart::g_wire.clear();
    g_r->set_generating_clock(true);
    g_r->realtime(Src::Uart, 0xF8u);
    TEST_ASSERT_TRUE_MESSAGE(uart::g_wire.empty(),
                             "an inbound clock was forwarded while the pedal generates one");

    // The pedal's own still leaves, which is the whole point of dropping the other.
    g_r->realtime(Src::Self, 0xF8u);
    TEST_ASSERT_TRUE(wire_is({0xF8}));
}

void test_a_realtime_byte_the_policy_refuses_never_reaches_the_jack(void) {
    configure(OutMode::Merge, UsbDinRoute::Off, /*clock_thru=*/false);
    g_r->realtime(Src::Uart, 0xF8u);                // clock, with thru off
    g_r->realtime(Src::Uart, 0xFEu);                // active sensing, always dropped
    TEST_ASSERT_TRUE(uart::g_wire.empty());

    g_r->realtime(Src::Uart, 0xFFu);                // system reset travels
    TEST_ASSERT_TRUE(wire_is({0xFF}));
}

// The pedal's own generated clock is not an echo, so it is judged by whether the jack
// carries the pedal's traffic at all -- not by clock_thru, and not suppressed by the
// pedal being the one generating.
void test_the_pedals_own_clock_is_not_judged_as_an_echo(void) {
    configure(OutMode::Out, UsbDinRoute::Off, /*clock_thru=*/false);
    g_r->set_generating_clock(true);
    g_r->realtime(Src::Self, 0xF8u);
    TEST_ASSERT_TRUE_MESSAGE(wire_is({0xF8}),
                             "the generated clock was judged by the echo's rules");

    uart::g_wire.clear();
    configure(OutMode::Thru);                       // the jack carries no own traffic
    g_r->realtime(Src::Self, 0xF8u);
    TEST_ASSERT_TRUE(uart::g_wire.empty());
}

// ---------------------------------------------------------------------------
// Running status
// ---------------------------------------------------------------------------

// A channel message whose status is already on the wire goes out as its data bytes
// alone. Both jacks run at 31250 baud, so a forwarded stream longer than the one
// arriving cannot be sustained at all.
void test_running_status_is_held_across_messages(void) {
    configure(OutMode::Merge);
    send(Src::Uart, {0xB0, 0x07, 0x40});
    send(Src::Uart, {0xB0, 0x07, 0x41});
    send(Src::Uart, {0xB0, 0x07, 0x42});
    TEST_ASSERT_TRUE(wire_is({0xB0, 0x07, 0x40, 0x07, 0x41, 0x07, 0x42}));
}

void test_a_new_status_breaks_the_run(void) {
    configure(OutMode::Merge);
    send(Src::Uart, {0xB0, 0x07, 0x40});
    send(Src::Uart, {0x90, 0x40, 0x7F});
    TEST_ASSERT_TRUE(wire_is({0xB0, 0x07, 0x40, 0x90, 0x40, 0x7F}));
}

// System Common breaks the run; the spec says so.
void test_system_common_breaks_the_run(void) {
    configure(OutMode::Merge);
    send(Src::Uart, {0xB0, 0x07, 0x40});
    send(Src::Uart, {0xF2, 0x00, 0x10});            // song position
    send(Src::Uart, {0xB0, 0x07, 0x41});
    TEST_ASSERT_TRUE(wire_is({0xB0, 0x07, 0x40, 0xF2, 0x00, 0x10, 0xB0, 0x07, 0x41}));
}

// A routing change is a claim about what the device downstream has been told, and it may
// have been told nothing, so the next message re-states its status.
void test_a_routing_change_forgets_the_status_on_the_wire(void) {
    configure(OutMode::Merge);
    send(Src::Uart, {0xB0, 0x07, 0x40});
    configure(OutMode::Merge);                      // any set_config, same values
    send(Src::Uart, {0xB0, 0x07, 0x41});
    TEST_ASSERT_TRUE(wire_is({0xB0, 0x07, 0x40, 0xB0, 0x07, 0x41}));
}

// ---------------------------------------------------------------------------
// The lock, and the queue behind it
// ---------------------------------------------------------------------------

// A frame streams through byte by byte -- a firmware image passing down the chain is why
// it streams rather than buffers -- and holds the jack while it does.
void test_a_streaming_frame_holds_the_jack(void) {
    configure(OutMode::Merge);
    TEST_ASSERT_TRUE(g_r->sysex_begin(Src::Uart, 1000u));
    g_r->sysex_byte(0xF0u, 1000u);
    g_r->sysex_byte(0x7Du, 1001u);

    // The pedal's own message cannot splice into it, so it waits.
    send(Src::Self, {0xB0, 0x07, 0x40});
    TEST_ASSERT_TRUE(wire_is({0xF0, 0x7D}));

    g_r->sysex_end(Src::Uart, /*write_eox=*/true);
    TEST_ASSERT_TRUE(wire_is({0xF0, 0x7D, 0xF7, 0xB0, 0x07, 0x40}));
}

// A second frame arriving mid-stream is refused outright: the jack cannot carry both,
// and 31250 baud could not fit them anyway.
void test_a_second_frame_is_refused_while_one_streams(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    TEST_ASSERT_TRUE(g_r->sysex_begin(Src::Uart, 1000u));
    TEST_ASSERT_FALSE(g_r->sysex_begin(Src::Usb, 1000u));

    g_r->sysex_end(Src::Uart, true);
    TEST_ASSERT_TRUE(g_r->sysex_begin(Src::Usb, 1002u));   // the jack is free again
}

// Only the owner can end the frame it started.
void test_only_the_owner_ends_the_frame(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    g_r->sysex_begin(Src::Uart, 1000u);
    g_r->sysex_byte(0xF0u, 1000u);

    g_r->sysex_end(Src::Usb, true);          // not the owner: ignored
    send(Src::Self, {0xB0, 0x07, 0x40});
    TEST_ASSERT_TRUE(wire_is({0xF0}));              // still locked, still queued

    g_r->sysex_end(Src::Uart, true);
    TEST_ASSERT_TRUE(wire_is({0xF0, 0xF7, 0xB0, 0x07, 0x40}));
}

// Queued messages leave in the order they arrived, each one whole.
void test_the_queue_drains_in_order(void) {
    configure(OutMode::Merge);
    g_r->sysex_begin(Src::Uart, 1000u);
    send(Src::Self, {0x90, 0x40, 0x7F});
    send(Src::Self, {0x80, 0x40, 0x00});
    g_r->sysex_end(Src::Uart, false);
    TEST_ASSERT_TRUE(wire_is({0x90, 0x40, 0x7F, 0x80, 0x40, 0x00}));
}

// Real-time bytes are legal anywhere in the stream, including inside a frame, so they
// pass the lock untouched rather than queueing behind it.
void test_realtime_passes_the_lock_untouched(void) {
    configure(OutMode::Merge);
    g_r->sysex_begin(Src::Uart, 1000u);
    g_r->sysex_byte(0xF0u, 1000u);
    g_r->realtime(Src::Uart, 0xF8u);                // a clock, mid-frame
    g_r->sysex_byte(0x7Du, 1001u);
    TEST_ASSERT_TRUE(wire_is({0xF0, 0xF8, 0x7D}));
}

// A message the queue cannot hold is dropped rather than truncated: half a message
// downstream is worse than none.
void test_a_message_too_large_for_the_queue_is_dropped_whole(void) {
    configure(OutMode::Merge);
    g_r->sysex_begin(Src::Uart, 1000u);

    // Offer far more than the queue can hold. The status alternates so running status
    // never applies: with every message re-stating its status, each one that survives is
    // three bytes on the wire, which is what makes whole-message-ness observable at all.
    // Held to one status they would compress to two bytes apiece and a truncated message
    // would be indistinguishable from a compressed one.
    const uint16_t offered = 200u;
    for (uint16_t i = 0; i < offered; ++i)
        send(Src::Self, {(uint8_t)((i & 1u) ? 0x90u : 0xB0u), 0x07u, (uint8_t)(i & 0x7Fu)});
    g_r->sysex_end(Src::Uart, false);

    // What got through is whole messages and nothing partial, and the overflow was
    // dropped rather than truncated onto the wire.
    TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(uart::g_wire.size() % 3u));
    TEST_ASSERT_TRUE(uart::g_wire.size() > 0u);
    TEST_ASSERT_TRUE(uart::g_wire.size() < (size_t)offered * 3u);   // some were refused

    // And every surviving message is intact, not a spliced pair.
    for (size_t i = 0; i + 2u < uart::g_wire.size(); i += 3u) {
        const uint8_t st = uart::g_wire[i];
        TEST_ASSERT_TRUE_MESSAGE(st == 0x90u || st == 0xB0u, "a message lost its status byte");
        TEST_ASSERT_EQUAL_UINT8(0x07u, uart::g_wire[i + 1u]);
    }
}

// ---------------------------------------------------------------------------
// The stall timeout
// ---------------------------------------------------------------------------

// A DIN sender pushes bytes 320 us apart and a host's packets are far closer together
// than the timeout, so only a frame that has stopped coming trips it -- an unplugged
// cable mid-dump being the case that matters.
void test_a_frame_that_stops_coming_gives_the_jack_back(void) {
    configure(OutMode::Merge);
    g_r->sysex_begin(Src::Uart, 1000u);
    g_r->sysex_byte(0xF0u, 1000u);
    send(Src::Self, {0xB0, 0x07, 0x40});            // waiting behind the frame

    Src owner = Src::Self;
    TEST_ASSERT_FALSE(g_r->poll(1500u, owner));     // still within the window
    TEST_ASSERT_TRUE(g_r->stalled(2000u));          // a second of silence

    TEST_ASSERT_TRUE(g_r->poll(2000u, owner));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)Src::Uart, (int)owner,
                                  "poll did not name the stream that stalled");

    // The forwarded copy is closed so the downstream parser is not left holding a frame
    // that never ends, and what queued behind it goes out.
    TEST_ASSERT_TRUE(wire_is({0xF0, 0xF7, 0xB0, 0x07, 0x40}));

    // The jack is free, and there is nothing left to take back.
    TEST_ASSERT_FALSE(g_r->poll(9000u, owner));
    TEST_ASSERT_TRUE(g_r->sysex_begin(Src::Usb, 9000u));
}

// Every byte says the frame is alive, so a slow but live sender never trips it.
void test_a_live_frame_never_stalls(void) {
    configure(OutMode::Merge);
    g_r->sysex_begin(Src::Uart, 1000u);
    for (uint32_t t = 1000u; t < 5000u; t += 500u) {
        g_r->sysex_byte(0x00u, t);
        TEST_ASSERT_FALSE(g_r->stalled(t + 499u));
    }
}

// Nothing is holding the jack, so nothing can stall.
void test_an_idle_jack_does_not_stall(void) {
    configure(OutMode::Merge);
    TEST_ASSERT_FALSE(g_r->stalled(100000u));
}

// poll() names the stream whose frame stalled, because the caller has a parser of its own
// to stop forwarding from -- and that is its state rather than the jack's.
void test_poll_names_the_stream_whose_frame_stalled(void) {
    configure(OutMode::Merge, UsbDinRoute::UsbToDin);
    g_r->sysex_begin(Src::Usb, 1000u);
    Src owner = Src::Uart;
    TEST_ASSERT_TRUE(g_r->poll(2000u, owner));
    TEST_ASSERT_EQUAL_INT((int)Src::Usb, (int)owner);
}

// A fresh router holds nothing: no lock, no queue, no status on the wire.
void test_a_fresh_router_holds_nothing(void) {
    configure(OutMode::Merge);
    send(Src::Uart, {0xB0, 0x07, 0x40});

    DinRouter fresh;
    uart::g_wire.clear();
    Config c; c.out_mode = OutMode::Merge;
    fresh.set_config(c);
    const uint8_t msg[3] = { 0xB0u, 0x07u, 0x41u };
    fresh.message(Src::Uart, msg, 3u);
    TEST_ASSERT_TRUE_MESSAGE(wire_is({0xB0, 0x07, 0x41}),
                             "a fresh router inherited a status byte from another");
    Src owner = Src::Uart;
    TEST_ASSERT_FALSE(fresh.poll(100000u, owner));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_inbound_stream_is_echoed_on_merge_and_thru);
    RUN_TEST(test_the_pedals_own_traffic_leaves_on_merge_and_out);
    RUN_TEST(test_usb_reaches_din_only_when_both_switches_agree);
    RUN_TEST(test_din_reaches_usb_only_on_the_cross_routes_that_point_that_way);
    RUN_TEST(test_active_sensing_is_dropped_on_every_setting);
    RUN_TEST(test_the_clock_family_rides_the_clock_thru_switch);
    RUN_TEST(test_a_generated_clock_suppresses_the_inbound_one);
    RUN_TEST(test_system_reset_travels_with_the_echo);
    RUN_TEST(test_a_message_the_policy_refuses_never_reaches_the_jack);
    RUN_TEST(test_usb_traffic_reaches_the_jack_only_where_the_cross_route_says);
    RUN_TEST(test_a_usb_realtime_byte_obeys_the_cross_route);
    RUN_TEST(test_an_inbound_clock_is_dropped_while_the_pedal_generates);
    RUN_TEST(test_a_realtime_byte_the_policy_refuses_never_reaches_the_jack);
    RUN_TEST(test_the_pedals_own_clock_is_not_judged_as_an_echo);
    RUN_TEST(test_running_status_is_held_across_messages);
    RUN_TEST(test_a_new_status_breaks_the_run);
    RUN_TEST(test_system_common_breaks_the_run);
    RUN_TEST(test_a_routing_change_forgets_the_status_on_the_wire);
    RUN_TEST(test_a_streaming_frame_holds_the_jack);
    RUN_TEST(test_a_second_frame_is_refused_while_one_streams);
    RUN_TEST(test_only_the_owner_ends_the_frame);
    RUN_TEST(test_the_queue_drains_in_order);
    RUN_TEST(test_realtime_passes_the_lock_untouched);
    RUN_TEST(test_a_message_too_large_for_the_queue_is_dropped_whole);
    RUN_TEST(test_a_frame_that_stops_coming_gives_the_jack_back);
    RUN_TEST(test_a_live_frame_never_stalls);
    RUN_TEST(test_an_idle_jack_does_not_stall);
    RUN_TEST(test_poll_names_the_stream_whose_frame_stalled);
    RUN_TEST(test_a_fresh_router_holds_nothing);
    return UNITY_END();
}
