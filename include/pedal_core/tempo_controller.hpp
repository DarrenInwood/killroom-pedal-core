#pragma once
#include <cstdint>
#include "pedal_core_tempo_config.hpp"   // BPM_DEFAULT
#include <pedal_core/ialgorithm.hpp>

using pedal_core::IAlgorithm;

// How the last tempo change was made — drives the '*' in the BPM readout (Midi → '*').
enum class TempoSource : uint8_t { Param, Tap, Midi };

// TempoController — the ephemeral-tempo state machine. The tempo-synced parameter is the
// single source of truth for tempo; this owns only what to show (the displayed BPM and
// which source last set it, for the '*') and whether incoming MIDI clock may still drive
// it. It calls tap_tempo (the tap/clock engine) and display_manager (the readout); the
// affected algorithm is passed in, so it has no registry dependency. The preset's
// MIDI-sync enable lives in Pedal and is passed in as `sync_on`.
class TempoController {
public:
    // Bring up the tap/clock engine. Call once at boot, before any other method.
    void init();

    // Preset load: arm MIDI iff the preset enables sync. If it does and the pedal is
    // already receiving clock, seed the parameter from the live MIDI tempo and show the
    // '*'; otherwise derive the displayed tempo from the just-loaded parameter.
    void on_preset_load(bool sync_on, IAlgorithm& a);

    // A live MIDI-sync enable/disable (the P4 knob or SysEx SET_SYNC). Re-arms MIDI iff sync
    // is now on, so turning Sync on locks to an already-running clock at once — without
    // waiting for a preset reload. Same arm-and-seed logic as a preset load.
    void on_sync_change(bool sync_on, IAlgorithm& a);

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

    // Manual edit of the tempo-synced parameter: re-derive the BPM and take over from MIDI.
    void on_param_edit(IAlgorithm& a);

    // SysEx Set-BPM: behaves like a tap (drive the parameter, take over from MIDI).
    void on_set_bpm(IAlgorithm& a, float bpm);

    // One MIDI clock pulse. Always advances the clock/sync tracker; drives the parameter
    // only while the preset enables sync, MIDI is still the armed source (no tap/edit since
    // load), and the clock is actually synced.
    void on_midi_clock(bool sync_on, IAlgorithm& a);

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

private:
    // Clamp the BPM to the shared limits and push it to the readout; '*' shown when Midi.
    void show(float bpm, TempoSource src);

    TempoSource m_tempo_source = TempoSource::Param;
    float       m_display_bpm  = BPM_DEFAULT;
    bool        m_midi_armed   = false;
};
