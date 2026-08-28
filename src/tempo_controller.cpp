#if __has_include("pedal_core_tempo_config.hpp")

#include <pedal_core/tempo_controller.hpp>
#include <pedal_core/tap_tempo.hpp>
#include <pedal_core/tempo_led.hpp>

void TempoController::init()
{
    tap_tempo::init();
}

// Clamp to the shared BPM limits so a value derived from a parameter (e.g. a very short
// delay) stays in a sane range. The tempo is conveyed by the Tempo LED, not the OLED;
// m_display_bpm is still tracked for the SysEx dump and the generated clock.
void TempoController::show(float bpm, TempoSource src)
{
    // An algorithm with no tempo parameter has no tempo of its own, and IAlgorithm's
    // tempo_bpm() answers 0 to say so. Leave the last tempo standing rather than clamping
    // that 0 up to BPM_MIN: loading a reverb preset must not reset the readout, or the
    // MIDI clock the pedal is mastering, to the bottom of the range.
    if (!(bpm > 0.0f)) return;
    if (bpm < BPM_MIN) bpm = BPM_MIN;
    else if (bpm > BPM_MAX) bpm = BPM_MAX;
    m_display_bpm  = bpm;
    m_tempo_source = src;
}

void TempoController::on_preset_load(bool sync_on, IAlgorithm& a)
{
    // A preset load establishes the arm state from the preset's sync flag against the
    // freshly-loaded parameter — identical logic to a live sync change.
    on_sync_change(sync_on, a);
}

void TempoController::on_sync_change(bool sync_on, IAlgorithm& a)
{
    // MIDI sync may drive tempo until a tap or affected-param edit overrides it, so arm iff
    // sync is on. Called on a preset load and on a live Sync toggle (P4 knob / SysEx
    // SET_SYNC), so enabling sync takes effect immediately instead of only on the next load.
    m_midi_armed = sync_on;
    if (sync_on && tap_tempo::is_midi_synced()) {
        // Sync is on and the pedal is currently synced — seed the affected parameter from
        // the live MIDI tempo and show it as synced.
        a.set_tempo(tap_tempo::get_midi_bpm());
        show(tap_tempo::get_midi_bpm(), TempoSource::Midi);
    } else {
        // Otherwise derive the displayed tempo from the current parameter.
        show(a.tempo_bpm(), TempoSource::Param);
    }
}

void TempoController::on_algo_change(IAlgorithm& a)
{
    // The LED is locked to the algorithm's own rate, so a change restarts its phase —
    // tempo_led.hpp asks for exactly this on an algorithm change.
    tempo_led::reset();
    // Hand over the tempo the tap/clock engine is holding. show() is deliberately not
    // called: the tempo did not change, only the algorithm reading it, so neither the
    // displayed BPM nor the '*' that names its source has anything new to say.
    a.set_tempo(tap_tempo::get_bpm());
}

bool TempoController::on_tap(IAlgorithm& a)
{
    if (!tap_tempo::tap()) return false;   // first / same-ms tap: nothing established
    a.set_tempo(tap_tempo::get_bpm());
    show(tap_tempo::get_bpm(), TempoSource::Tap);
    m_midi_armed = false;
    return true;
}

void TempoController::on_param_edit(IAlgorithm& a)
{
    show(a.tempo_bpm(), TempoSource::Param);
    m_midi_armed = false;
}

void TempoController::on_set_bpm(IAlgorithm& a, float bpm)
{
    // Behaves like a tap: set_bpm clamps to [BPM_MIN, BPM_MAX], so drive the parameter and
    // the readout from the clamped value (tap_tempo::get_bpm()) — exactly as on_tap does —
    // rather than the raw argument. Otherwise an out-of-range SysEx Set-BPM would retime the
    // effect from an unclamped tempo while the display and saved bpm_x10 show the clamped one.
    tap_tempo::set_bpm(bpm);
    const float clamped = tap_tempo::get_bpm();
    a.set_tempo(clamped);
    show(clamped, TempoSource::Tap);
    m_midi_armed = false;
}

bool TempoController::midi_driving(bool sync_on) const
{
    return sync_on && m_midi_armed && tap_tempo::is_midi_synced();
}

void TempoController::on_midi_clock(bool sync_on, IAlgorithm& a)
{
    // Always track clock tempo + sync status, even when the current preset doesn't use
    // MIDI sync, so a later preset load can consult is_midi_synced()/get_midi_bpm().
    tap_tempo::midi_clock_tick();
    if (!midi_driving(sync_on)) return;
    a.set_tempo(tap_tempo::get_midi_bpm());
    show(tap_tempo::get_midi_bpm(), TempoSource::Midi);
}

void TempoController::on_midi_clock_reset()
{
    // Always honour start/stop so sync status is cleared regardless of preset setting.
    tap_tempo::midi_clock_reset();
}

#endif  // __has_include(pedal_core_tempo_config.hpp)
