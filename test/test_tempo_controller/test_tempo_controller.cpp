// Host-native unit tests for TempoController (src/tempo_controller.cpp).
//
// TempoController is the ephemeral-tempo state machine: it ties tap tempo, a manual edit
// of the tempo-synced parameter, SysEx Set-BPM, MIDI clock, and preset-load arming into
// the displayed BPM + which source last set it (the '*') + whether MIDI may still drive
// it. It calls the real tap_tempo engine (in the native build_src_filter); the
// affected algorithm is passed in, so a small
// FakeAlgorithm with a controllable tempo_bpm()/set_tempo() makes every path observable.

#include <unity.h>
#include <cstdint>

#include <pedal_core/tempo_controller.hpp>
#include <pedal_core/tap_tempo.hpp>
#include "pedal_core_tempo_config.hpp"

namespace systick {
    void fake_set_ms(uint32_t ms);
    void fake_advance_ms(uint32_t ms);
}


// A tempo-synced fake algorithm: records the last set_tempo() and reports a
// settable tempo_bpm(); tempo_param() is 0 (i.e. it has a synced parameter).
class FakeAlgorithm : public pedal_core::IAlgorithm {
public:
    float  m_last_set_tempo = -1.0f;
    float  m_tempo_bpm_val  = 120.0f;

    const char* name()       const override { return "Fake"; }
    uint8_t     num_params() const override { return 4; }
    uint16_t    get_param(uint8_t) const override { return 0; }
    const char* param_name(uint8_t) const override { return "P"; }
    void        param_display(uint8_t, uint16_t, char* buf, uint8_t len) const override
    {
        if (len) buf[0] = '\0';
    }
    uint16_t default_param(uint8_t) const override { return 64; }
    void     reset() override {}
    int8_t   tempo_param() const override { return 0; }
    float    tempo_bpm()   const override { return m_tempo_bpm_val; }
    void     set_tempo(float bpm) override { m_last_set_tempo = bpm; }
};

// An algorithm with no tempo-synced parameter — a reverb, a pitch shifter. IAlgorithm's
// tempo_bpm() defaults to 0, which is the interface saying "I have no tempo", and is not
// the same statement as a tempo of zero.
class UntimedAlgorithm : public FakeAlgorithm {
public:
    int8_t tempo_param() const override { return -1; }
    float  tempo_bpm()   const override { return 0.0f; }
};

static TempoController* g_tc = nullptr;
static FakeAlgorithm*   g_a  = nullptr;

// Feed one full MIDI-clock beat (24 pulses) at the given spacing with sync disabled, so
// tap_tempo latches "synced" without the controller driving the parameter.
static void track_clock_beat(uint32_t spacing_ms) {
    for (int i = 0; i < 24; ++i) {
        g_tc->on_midi_clock(/*sync_on=*/false, *g_a);
        systick::fake_advance_ms(spacing_ms);
    }
}

void setUp(void) {
        systick::fake_set_ms(1000);
    tap_tempo::init();
    g_tc = new TempoController();
    g_a  = new FakeAlgorithm();
}
void tearDown(void) { delete g_tc; delete g_a; g_tc = nullptr; g_a = nullptr; }

// ---------------------------------------------------------------------------

// A manual param edit re-derives the BPM from the parameter and disarms MIDI.
void test_param_edit_shows_param_bpm_and_disarms(void) {
    g_a->m_tempo_bpm_val = 137.0f;
    g_tc->on_param_edit(*g_a);
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Param, (int)g_tc->tempo_source());
    TEST_ASSERT_FALSE(g_tc->midi_armed());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 137.0f, g_tc->display_bpm());}

// Set-BPM behaves like a tap: drives the parameter, shows the value, disarms MIDI.
void test_set_bpm_drives_param_and_disarms(void) {
    g_tc->on_set_bpm(*g_a, 150.0f);
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Tap, (int)g_tc->tempo_source());
    TEST_ASSERT_FALSE(g_tc->midi_armed());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 150.0f, g_tc->display_bpm());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 150.0f, g_a->m_last_set_tempo);
}

// Set-BPM clamps like a tap: an out-of-range BPM (e.g. a large SysEx bpm_x10) drives the
// parameter AND the readout from the clamped value, so the retimed effect and the shown/saved
// tempo agree — the defect was set_tempo() getting the raw, unclamped argument.
void test_set_bpm_clamps_out_of_range(void) {
    g_tc->on_set_bpm(*g_a, 5000.0f);                // far above BPM_MAX
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MAX, g_tc->display_bpm());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MAX, g_a->m_last_set_tempo);

    g_tc->on_set_bpm(*g_a, 1.0f);                   // far below BPM_MIN
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MIN, g_tc->display_bpm());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MIN, g_a->m_last_set_tempo);
}

// A tap drives the parameter from the tapped tempo (two taps 400 ms apart -> 150 BPM).
void test_tap_drives_param(void) {
    g_tc->on_tap(*g_a);                 // first tap (no interval yet)
    systick::fake_advance_ms(400);
    g_tc->on_tap(*g_a);                 // second tap -> 150 BPM
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Tap, (int)g_tc->tempo_source());
    TEST_ASSERT_FALSE(g_tc->midi_armed());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 150.0f, g_tc->display_bpm());
}

// Preset load, not synced: derive the displayed tempo from the parameter; arm = sync flag.
void test_preset_load_not_synced_derives_from_param(void) {
    g_a->m_tempo_bpm_val = 96.0f;
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);   // sync on, but no clock yet
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Param, (int)g_tc->tempo_source());
    TEST_ASSERT_TRUE(g_tc->midi_armed());            // armed because the preset enables sync
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 96.0f, g_tc->display_bpm());}

// Preset load, sync on AND already synced: seed the parameter from the live MIDI tempo.
void test_preset_load_synced_seeds_from_midi(void) {
    track_clock_beat(20);                            // 480 ms/beat -> 125 BPM, latches synced
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());

    g_tc->on_preset_load(/*sync_on=*/true, *g_a);
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());
    TEST_ASSERT_TRUE(g_tc->midi_armed());    TEST_ASSERT_FLOAT_WITHIN(1.0f, 125.0f, g_tc->display_bpm());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 125.0f, g_a->m_last_set_tempo);
}

// Clock drives tempo only when the preset enables sync and MIDI is armed.
void test_clock_drives_when_sync_on_and_armed(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);    // armed, not yet synced
    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(/*sync_on=*/true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());    TEST_ASSERT_FLOAT_WITHIN(1.0f, 156.25f, g_a->m_last_set_tempo);  // 16 ms*24 = 384 ms/beat
}

void test_clock_ignored_when_sync_off(void) {
    g_tc->on_preset_load(/*sync_on=*/false, *g_a);   // not armed
    track_clock_beat(16);                            // sync_on=false on every pulse
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Param, (int)g_tc->tempo_source());}

// A tap takes over from MIDI: further clock pulses are ignored until re-armed.
void test_tap_overrides_clock(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);
    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());

    g_tc->on_tap(*g_a);                 // first tap: inert (MIDI still drives)
    systick::fake_advance_ms(400);
    g_tc->on_tap(*g_a);                 // second tap: measures a tempo, takes over
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Tap, (int)g_tc->tempo_source());
    TEST_ASSERT_FALSE(g_tc->midi_armed());

    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Tap, (int)g_tc->tempo_source());  // still Tap
}

void test_clock_reset_clears_sync(void) {
    track_clock_beat(20);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
    g_tc->on_midi_clock_reset();
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
}

// midi_driving() — the header ♪ indicator's source — is a *live* query, not a latch: it is
// true only while the preset enables sync, MIDI is armed, and clock is arriving, and it
// clears the moment sync is lost (Stop or clock timeout). tempo_source() latching Midi is
// exactly the stale state this must not reflect.
void test_midi_driving_tracks_live_sync(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);        // armed, not yet synced
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));   // no clock yet

    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_TRUE(g_tc->midi_driving(/*sync_on=*/true));    // synced + armed -> driving
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());

    // sync_on gate: toggling the preset's MIDI-sync enable off (P4/SysEx) stops driving even
    // though tempo_source() still latches Midi and clock is still live.
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/false));

    // MIDI Stop clears the clock-sync status -> no longer driving, but tempo_source() is
    // still Midi (the defect: the ♪ used to stay lit off this latch).
    g_tc->on_midi_clock_reset();
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());
}

// Clock-inactivity timeout: with no Stop message, is_midi_synced() expires after
// MIDI_SYNC_TIMEOUT_MS, so midi_driving() clears on its own once the master goes quiet.
void test_midi_driving_clears_on_clock_timeout(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);
    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_TRUE(g_tc->midi_driving(/*sync_on=*/true));

    systick::fake_advance_ms(MIDI_SYNC_TIMEOUT_MS + 1u);      // master stops sending clock
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));
}

// A tap that takes over from MIDI disarms the controller, so midi_driving() is false even
// while clock keeps arriving.
void test_midi_driving_false_after_tap_takeover(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);
    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_TRUE(g_tc->midi_driving(/*sync_on=*/true));

    g_tc->on_tap(*g_a);                 // first tap: inert
    systick::fake_advance_ms(400);
    g_tc->on_tap(*g_a);                 // second tap: measures a tempo, disarms MIDI
    TEST_ASSERT_FALSE(g_tc->midi_armed());
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));  // armed cleared, though clock live
}

// Regression: enabling MIDI Sync live (P4 knob / SysEx SET_SYNC) on a preset that stored
// sync off arms MIDI at once, so a running clock drives the tempo without a preset reload —
// the defect was that only on_preset_load armed, so a live "Sync On" was shown but inert.
void test_sync_change_enables_live_without_reload(void) {
    g_tc->on_preset_load(/*sync_on=*/false, *g_a);   // preset stores sync off -> not armed
    track_clock_beat(20);                            // clock streaming, latched synced (~125 BPM)
    TEST_ASSERT_FALSE(g_tc->midi_armed());
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));  // "Sync On" would not drive (the bug)

    g_tc->on_sync_change(/*sync_on=*/true, *g_a);    // user turns P4 Sync On
    TEST_ASSERT_TRUE(g_tc->midi_armed());
    TEST_ASSERT_TRUE(g_tc->midi_driving(/*sync_on=*/true));   // now actually driving
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Midi, (int)g_tc->tempo_source());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 125.0f, g_a->m_last_set_tempo);  // seeded from the live clock
}

// Enabling Sync live with no clock arriving arms MIDI but does not drive yet; the display
// stays parameter-derived until a clock actually locks.
void test_sync_change_enable_without_clock_arms_only(void) {
    g_a->m_tempo_bpm_val = 96.0f;
    g_tc->on_sync_change(/*sync_on=*/true, *g_a);    // no clock present
    TEST_ASSERT_TRUE(g_tc->midi_armed());
    TEST_ASSERT_FALSE(g_tc->midi_driving(/*sync_on=*/true));  // armed, but nothing to sync to
    TEST_ASSERT_EQUAL_INT((int)TempoSource::Param, (int)g_tc->tempo_source());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 96.0f, g_tc->display_bpm());
}

// Disabling Sync live disarms MIDI, so clock no longer drives even while it keeps arriving.
void test_sync_change_disable_disarms(void) {
    g_tc->on_preset_load(/*sync_on=*/true, *g_a);
    for (int i = 0; i < 24; ++i) { g_tc->on_midi_clock(true, *g_a); systick::fake_advance_ms(16); }
    TEST_ASSERT_TRUE(g_tc->midi_driving(/*sync_on=*/true));

    g_tc->on_sync_change(/*sync_on=*/false, *g_a);   // user turns Sync Off
    TEST_ASSERT_FALSE(g_tc->midi_armed());
}

// show() clamps a parameter-derived BPM into [BPM_MIN, BPM_MAX] for display, so a value
// from a very short (or very long) tempo-synced parameter still shows a sane tempo.
// on_param_edit routes through show(), so a fake reporting an out-of-range tempo_bpm()
// exercises both clamp branches.
void test_show_clamps_param_bpm_to_range(void) {
    g_a->m_tempo_bpm_val = 400.0f;                  // above BPM_MAX
    g_tc->on_param_edit(*g_a);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MAX, g_tc->display_bpm());

    g_a->m_tempo_bpm_val = 5.0f;                    // below BPM_MIN
    g_tc->on_param_edit(*g_a);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, BPM_MIN, g_tc->display_bpm());
}

// Loading a preset onto an algorithm that has no tempo of its own must leave the tempo
// where it was. The readout is one thing, but the same number feeds the clock this pedal
// generates for everything downstream, so treating "no tempo" as a tempo of zero would
// drop every slaved device to the bottom of the range on a reverb preset.
void test_an_algorithm_with_no_tempo_leaves_the_last_one_standing(void) {
    g_a->m_tempo_bpm_val = 0.0f;
    g_tc->on_tap(*g_a);                       // two taps to establish something
    systick::fake_advance_ms(500);
    g_tc->on_tap(*g_a);
    const float established = g_tc->display_bpm();
    TEST_ASSERT_TRUE(established > 0.0f);

    UntimedAlgorithm untimed;
    g_tc->on_preset_load(/*sync_on=*/false, untimed);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, established, g_tc->display_bpm());
}


// An algorithm change hands the incoming algorithm the tempo the pedal is already
// holding, so a sound selected mid-song arrives in time rather than at its own default.
void test_algo_change_hands_over_the_held_tempo(void) {
    // Establish a tapped tempo: two taps 500 ms apart is 120 BPM.
    g_tc->on_tap(*g_a);
    systick::fake_advance_ms(500);
    g_tc->on_tap(*g_a);

    FakeAlgorithm incoming;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, incoming.m_last_set_tempo);  // untouched
    g_tc->on_algo_change(incoming);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, tap_tempo::get_bpm(), incoming.m_last_set_tempo);
}

// Which algorithm is playing is not a statement about where the tempo came from, so the
// readout and the armed source are left exactly as they were.
void test_algo_change_leaves_the_readout_and_the_source_alone(void) {
    g_a->m_tempo_bpm_val = 137.0f;
    g_tc->on_param_edit(*g_a);            // Param source, MIDI disarmed, 137 showing
    const float       bpm = g_tc->display_bpm();
    const TempoSource src = g_tc->tempo_source();

    FakeAlgorithm incoming;
    g_tc->on_algo_change(incoming);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, bpm, g_tc->display_bpm());
    TEST_ASSERT_EQUAL_INT((int)src, (int)g_tc->tempo_source());
    TEST_ASSERT_FALSE(g_tc->midi_armed());
}

// Arming is likewise untouched: a preset with sync on stays armed across a change of
// algorithm, so the next clock pulse still drives the tempo.
void test_algo_change_leaves_midi_armed(void) {
    g_tc->on_sync_change(true, *g_a);
    TEST_ASSERT_TRUE(g_tc->midi_armed());
    FakeAlgorithm incoming;
    g_tc->on_algo_change(incoming);
    TEST_ASSERT_TRUE(g_tc->midi_armed());
}
int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_param_edit_shows_param_bpm_and_disarms);
    RUN_TEST(test_set_bpm_drives_param_and_disarms);
    RUN_TEST(test_set_bpm_clamps_out_of_range);
    RUN_TEST(test_tap_drives_param);
    RUN_TEST(test_preset_load_not_synced_derives_from_param);
    RUN_TEST(test_preset_load_synced_seeds_from_midi);
    RUN_TEST(test_clock_drives_when_sync_on_and_armed);
    RUN_TEST(test_clock_ignored_when_sync_off);
    RUN_TEST(test_tap_overrides_clock);
    RUN_TEST(test_clock_reset_clears_sync);
    RUN_TEST(test_midi_driving_tracks_live_sync);
    RUN_TEST(test_midi_driving_clears_on_clock_timeout);
    RUN_TEST(test_midi_driving_false_after_tap_takeover);
    RUN_TEST(test_sync_change_enables_live_without_reload);
    RUN_TEST(test_sync_change_enable_without_clock_arms_only);
    RUN_TEST(test_sync_change_disable_disarms);
    RUN_TEST(test_show_clamps_param_bpm_to_range);
    RUN_TEST(test_an_algorithm_with_no_tempo_leaves_the_last_one_standing);
    RUN_TEST(test_algo_change_hands_over_the_held_tempo);
    RUN_TEST(test_algo_change_leaves_the_readout_and_the_source_alone);
    RUN_TEST(test_algo_change_leaves_midi_armed);
    return UNITY_END();
}
