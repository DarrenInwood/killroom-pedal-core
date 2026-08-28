#if __has_include("pedal_core_tempo_config.hpp")

#include <pedal_core/tempo_controller.hpp>
#include <pedal_core/tap_tempo.hpp>
#include <pedal_core/tempo_led.hpp>
#include <pedal_core/midi_clock_out.hpp>

// Each event has one body, working from the bound algorithm and the held sync flag. The
// forms that take an algorithm bind it and delegate, so the two ways in cannot drift.

void TempoController::init()
{
    tap_tempo::init();
    m_last_tick_ms = 0u;
    m_ticked       = false;
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
    bind(a);
    on_preset_load(sync_on);
}

void TempoController::on_preset_load(bool sync_on)
{
    // A preset load establishes the arm state from the preset's sync flag against the
    // freshly-loaded parameter — identical logic to a live sync change.
    on_sync_change(sync_on);
}

void TempoController::on_sync_change(bool sync_on, IAlgorithm& a)
{
    bind(a);
    on_sync_change(sync_on);
}

void TempoController::on_sync_change(bool sync_on)
{
    // MIDI sync may drive tempo until a tap or affected-param edit overrides it, so arm iff
    // sync is on. Called on a preset load and on a live Sync toggle (P4 knob / SysEx
    // SET_SYNC), so enabling sync takes effect immediately instead of only on the next load.
    m_sync_on    = sync_on;
    m_midi_armed = sync_on;
    if (m_algo == nullptr) return;

    if (sync_on && tap_tempo::is_midi_synced()) {
        // Sync is on and the pedal is currently synced — seed the affected parameter from
        // the live MIDI tempo and show it as synced.
        m_algo->set_tempo(tap_tempo::get_midi_bpm());
        show(tap_tempo::get_midi_bpm(), TempoSource::Midi);
    } else {
        // Otherwise derive the displayed tempo from the current parameter.
        show(m_algo->tempo_bpm(), TempoSource::Param);
    }
}

void TempoController::on_algo_change(IAlgorithm& a)
{
    bind(a);
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
    bind(a);
    return on_tap();
}

bool TempoController::on_tap()
{
    if (!tap_tempo::tap()) return false;   // first / same-ms tap: nothing established
    if (m_algo != nullptr) m_algo->set_tempo(tap_tempo::get_bpm());
    show(tap_tempo::get_bpm(), TempoSource::Tap);
    m_midi_armed = false;
    return true;
}

void TempoController::on_param_edit(IAlgorithm& a)
{
    bind(a);
    on_param_edit();
}

void TempoController::on_param_edit()
{
    if (m_algo != nullptr) show(m_algo->tempo_bpm(), TempoSource::Param);
    m_midi_armed = false;
}

void TempoController::on_set_bpm(IAlgorithm& a, float bpm)
{
    bind(a);
    on_set_bpm(bpm);
}

void TempoController::on_set_bpm(float bpm)
{
    // Behaves like a tap: set_bpm clamps to [BPM_MIN, BPM_MAX], so drive the parameter and
    // the readout from the clamped value (tap_tempo::get_bpm()) — exactly as on_tap does —
    // rather than the raw argument. Otherwise an out-of-range SysEx Set-BPM would retime the
    // effect from an unclamped tempo while the display and saved bpm_x10 show the clamped one.
    tap_tempo::set_bpm(bpm);
    const float clamped = tap_tempo::get_bpm();
    if (m_algo != nullptr) m_algo->set_tempo(clamped);
    show(clamped, TempoSource::Tap);
    m_midi_armed = false;
}

bool TempoController::midi_driving(bool sync_on) const
{
    return sync_on && m_midi_armed && tap_tempo::is_midi_synced();
}

void TempoController::clock_tick(bool sync_on)
{
    // Always track clock tempo + sync status, even when the current preset doesn't use
    // MIDI sync, so a later preset load can consult is_midi_synced()/get_midi_bpm().
    tap_tempo::midi_clock_tick();
    if (!midi_driving(sync_on) || m_algo == nullptr) return;
    m_algo->set_tempo(tap_tempo::get_midi_bpm());
    show(tap_tempo::get_midi_bpm(), TempoSource::Midi);
}

void TempoController::on_midi_clock(bool sync_on, IAlgorithm& a)
{
    bind(a);
    // `sync_on` is a per-call override and does not become the held flag: what the preset
    // says is on_preset_load()'s and on_sync_change()'s to say.
    clock_tick(sync_on);
}

void TempoController::on_midi_clock()
{
    clock_tick(m_sync_on);
}

void TempoController::on_midi_clock_reset()
{
    // Always honour start/stop so sync status is cleared regardless of preset setting.
    tap_tempo::midi_clock_reset();
}

void TempoController::tick(uint32_t now_ms, bool idle_on)
{
    // The LED runs on elapsed time. The first tick has no interval behind it, so it
    // contributes nothing to the beat rather than a jump the length of the boot.
    uint32_t dt_ms = 0u;
    if (m_ticked) dt_ms = (uint32_t)(now_ms - m_last_tick_ms);
    m_ticked       = true;
    m_last_tick_ms = now_ms;

    // The beat the LED locks to is the algorithm's, which may be a fraction of a beat;
    // 0 means it has none, and the LED shows `idle_on` instead.
    const uint32_t period_ms = (m_algo != nullptr) ? m_algo->beat_period_ms() : 0u;
    tempo_led::update((float)dt_ms, period_ms, idle_on);

    // Exactly one clock leaves the jack. The generator stops while an incoming clock is
    // driving the tempo, so the pedal forwards that one rather than adding a second of
    // its own — an invariant rather than a setting a player has to get right. Whether it
    // generates at all is the player's setting, and stays with midi_clock_out.
    midi_clock_out::set_bpm(m_display_bpm);
    midi_clock_out::set_running(!midi_driving());
    midi_clock_out::poll(now_ms);
}

#endif  // __has_include(pedal_core_tempo_config.hpp)
