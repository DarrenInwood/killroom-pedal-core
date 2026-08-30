#pragma once
#include <cstdint>
#include "pedal_core_features.hpp"

// Reached by a product that did not ask for the tempo layer, this header would fail on the
// config include below with nothing to say about why. Saying it here costs one branch and
// turns "no such file" into the question actually worth answering: whether this product
// wants a tempo at all.
#if !PEDAL_CORE_HAS_TEMPO
#  error "tempo_controller.hpp needs PEDAL_CORE_HAS_TEMPO. Set it in pedal_core_features.hpp and supply pedal_core_tempo_config.hpp."
#endif

#include "pedal_core_tempo_config.hpp"   // BPM_DEFAULT
#include <pedal_core/ialgorithm.hpp>

using pedal_core::IAlgorithm;

// How the last tempo change was made — drives the '*' in the BPM readout (Midi → '*').
enum class TempoSource : uint8_t { Param, Tap, Midi };

// TempoController — the tempo layer. The tempo-synced parameter is the single source of
// truth for tempo; this owns what to show (the displayed BPM and which source last set
// it, for the '*'), whether incoming MIDI clock may still drive it, and — through tick()
// — the two things that run off the tempo: the tempo LED and the generated MIDI clock.
//
// It calls tap_tempo (the tap/clock engine), tempo_led and midi_clock_out. It has no
// registry dependency: the affected algorithm is bound, and the preset's MIDI-sync enable
// is held from the two events that change it.
//
// TWO WAYS IN, ONE STATE
//
// Every event has a form that takes the algorithm (and, where it matters, the sync flag)
// and a form that takes neither. The second reads what the first has already established:
// every entry point binds the algorithm it is given, and on_preset_load()/on_sync_change()
// record the sync flag, so a caller can move to the shorter forms an event at a time
// without the two disagreeing.
//
// Passing a different `sync_on` to on_midi_clock() than to midi_driving() is the mistake
// the shorter forms exist to make impossible. The longer form keeps its argument as a
// per-call override and does not rewrite the held flag: what the preset says is
// on_preset_load()'s and on_sync_change()'s to say.
class TempoController {
public:
    // Bring up the tap/clock engine. Call once at boot, before any other method.
    void init();

    // Hold the algorithm the tempo drives, so the events below need not be handed it
    // each time. Every other entry point that takes an algorithm binds it too, so this
    // is for the caller that wants to bind once at boot and never pass one again.
    void bind(IAlgorithm& a) { m_algo = &a; }

    // Run the tempo layer for this loop pass: flash the LED at the algorithm's beat and
    // keep the generated clock paced and correctly silent. Call every main-loop tick.
    //
    // `idle_on` is what to show on the tempo LED where the algorithm has no beat — a
    // state the footswitch beside it is holding, on a product whose switches can carry
    // one. It is a parameter rather than a hook because only the product knows it, and
    // a hook would cost every product a derived type for the sake of one bool.
    //
    // Whether the pedal generates a clock at all is the player's setting and stays with
    // midi_clock_out::set_enabled(); what this owns is that exactly one clock leaves the
    // jack — the generator is stopped while an incoming clock is driving the tempo, so
    // the pedal forwards that one instead of adding a second.
    void tick(uint32_t now_ms, bool idle_on = false);

    // Preset load: arm MIDI iff the preset enables sync. If it does and the pedal is
    // already receiving clock, seed the parameter from the live MIDI tempo and show the
    // '*'; otherwise derive the displayed tempo from the just-loaded parameter.
    void on_preset_load(bool sync_on, IAlgorithm& a);
    void on_preset_load(bool sync_on);

    // A live MIDI-sync enable/disable (the P4 knob or SysEx SET_SYNC). Re-arms MIDI iff sync
    // is now on, so turning Sync on locks to an already-running clock at once — without
    // waiting for a preset reload. Same arm-and-seed logic as a preset load.
    void on_sync_change(bool sync_on, IAlgorithm& a);
    void on_sync_change(bool sync_on);

    // The live algorithm changed. Restart the tempo LED so its beat phase starts at the
    // new sound rather than mid-flash, and hand that algorithm the tempo the pedal is
    // already holding, so it arrives in time instead of at its own default. The displayed
    // BPM and the armed source are untouched: which algorithm is playing is not a statement
    // about where the tempo came from.
    void on_algo_change(IAlgorithm& a);

    // Tap tempo: on a tap that measures a new tempo (>=2 taps), drive the parameter from the
    // tapped BPM and take over from MIDI sync; returns true. A lone first tap is inert (no
    // tempo change, MIDI sync untouched) and returns false.
    bool on_tap(IAlgorithm& a);
    bool on_tap();

    // Manual edit of the tempo-synced parameter: re-derive the BPM and take over from MIDI.
    void on_param_edit(IAlgorithm& a);
    void on_param_edit();

    // SysEx Set-BPM: behaves like a tap (drive the parameter, take over from MIDI).
    void on_set_bpm(IAlgorithm& a, float bpm);
    void on_set_bpm(float bpm);

    // One MIDI clock pulse. Always advances the clock/sync tracker; drives the parameter
    // only while the preset enables sync, MIDI is still the armed source (no tap/edit since
    // load), and the clock is actually synced.
    void on_midi_clock(bool sync_on, IAlgorithm& a);
    void on_midi_clock();

    // MIDI start/stop: clear the clock-sync status.
    void on_midi_clock_reset();

    float       display_bpm()  const { return m_display_bpm; }
    TempoSource tempo_source() const { return m_tempo_source; }
    bool        midi_armed()   const { return m_midi_armed; }

    // True while incoming MIDI clock is currently driving the tempo: the preset enables
    // sync (`sync_on`), MIDI is still the armed source (no tap/edit since load), and the
    // clock is live (`tap_tempo::is_midi_synced()`, which expires on Stop/timeout). This is
    // the exact gate on_midi_clock() uses to drive the parameter, so a live query tracks
    // when the tempo is actually slaved — unlike tempo_source(), which latches Midi and only
    // records which source *last* set the tempo.
    bool        midi_driving(bool sync_on) const;
    bool        midi_driving() const { return midi_driving(m_sync_on); }

    // Whether the preset the pedal is holding enables MIDI sync, as the last
    // on_preset_load() or on_sync_change() left it.
    bool        sync_on() const { return m_sync_on; }

private:
    // Clamp the BPM to the shared limits and push it to the readout; '*' shown when Midi.
    void show(float bpm, TempoSource src);

    // One clock pulse against a sync flag, whether that flag was passed or is the held one.
    void clock_tick(bool sync_on);

    TempoSource m_tempo_source = TempoSource::Param;
    float       m_display_bpm  = BPM_DEFAULT;
    bool        m_midi_armed   = false;

    // The two facts the events would otherwise be handed every time.
    IAlgorithm* m_algo    = nullptr;
    bool        m_sync_on = false;

    // The LED runs on elapsed time, so the first tick has no interval to report and
    // contributes nothing to the beat rather than a jump the length of the boot.
    uint32_t    m_last_tick_ms = 0u;
    bool        m_ticked       = false;
};
