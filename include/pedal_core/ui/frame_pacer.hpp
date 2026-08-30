#pragma once
#include <cstdint>
#include "pedal_core_ui_config.hpp"   // DISPLAY_PARAM_SHOW_MS

// When the screen redraws, and what it should show when it does.
//
// The compositor paints; this decides whether painting is due. Everything that makes
// that a question rather than a yes lives here: the idle cap, the faster cadence while
// something animates, the transient overlays and their dwell, the boot splash and the
// storage-fault hold that precedes it, and the slide transition between screens.
//
// It knows time and nothing else. Every entry point is given `now` rather than reading a
// clock, and the one fact it cannot work out — whether the display is still flushing the
// last frame — arrives as a bool. So the whole machine runs on a host with no framebuffer,
// no driver and no fakes, which is what makes the pacing testable at all.
//
// The pixel arithmetic stays with the compositor: a decision carries a duration, and the
// caller turns that into an offset across its own screen width.
namespace pedal_core::ui {

class FramePacer {
public:
    // Which transient is up. The performance screen and the product's own screens are all
    // Overlay::None — what is underneath a transient is the compositor's business.
    enum class Overlay : uint8_t { None, Panel, Banner, Save };

    // What this tick should do.
    enum class What : uint8_t {
        Nothing,        // not due; the frame on the glass still stands
        SplashFrame,    // draw the splash art; arg = ms since it began
        SplashRestart,  // the fault hold is over, and the splash follows it
        SlideStep,      // compose the transition; arg = ms into SLIDE_MS
        SlideSettle,    // the transition is over; show the destination frame
        Frame,          // the ordinary composed frame, nothing over it
        FramePanel,     // the frame with the param focus panel; arg = unroll progress 0..256
        FrameBanner,    // the frame with a message banner; arg = progress 0..256
        FrameSave,      // the save confirmation, which owns the whole screen; no arg
    };

    struct Decision {
        What     what = What::Nothing;
        uint32_t arg  = 0u;
        // Render the destination screen and capture it before acting on `what`. A slide
        // needs its "to" frame in hand, and the tick that starts one may also be the tick
        // that draws its first step — which is why this rides alongside `what` rather
        // than being a `What` of its own.
        bool     capture_slide_target = false;
    };

    // What a slide can do about the screen it is leaving.
    enum class SlideStart : uint8_t {
        Refused,      // a splash is up or a slide is already running; nothing to do
        Ready,        // capture the current frame as the "from"
        RedrawFirst,  // a transient was open and has been closed: redraw, then capture
    };

    // How long the transition itself runs. Public because the caller turns a decision's
    // elapsed time into an offset across its own screen width.
    static constexpr uint32_t SLIDE_MS = 150u;

    // --- what the producers say ---------------------------------------------

    // Something the frame draws from has changed.
    void changed() { m_dirty = true; }

    // A transient opened. Re-opening the panel while it is already up (a stream of knob
    // updates) starts the clock past the unroll, so the value and the dwell refresh
    // without the panel replaying its opening every time the knob moves.
    void overlay(Overlay kind, uint32_t now)
    {
        const bool already_open = (kind == Overlay::Panel && m_overlay == Overlay::Panel);
        m_overlay          = kind;
        m_overlay_start_ms = already_open ? (uint32_t)(now - ANIM_MS) : now;
        m_dirty            = true;
    }

    // The boot splash, drawn and flushed by the caller before this is called: the first
    // frame goes up at once and the superloop animates the rest.
    void splash(uint32_t now)
    {
        m_splash_start_ms  = now;
        m_splash_expiry_ms = now + SPLASH_MS;
        m_splash_active    = true;
        m_last_draw_ms     = now;
        m_dirty            = false;
    }

    // The storage-fault warning: a static hold, on the same expiry the splash uses, with
    // the splash queued behind it so a faulted boot still runs fault, splash, then the UI.
    void fault_hold(uint32_t now)
    {
        m_splash_expiry_ms  = now + FAULT_HOLD_MS;
        m_splash_active     = false;
        m_splash_after_hold = true;
        m_dirty             = false;
    }

    // A screen change wants to slide. Refused while a splash holds the screen or another
    // slide is in flight. An open transient is closed here rather than sliding with the
    // frame: it would animate as part of the transition and then reappear over the screen
    // it landed on, so the caller redraws without it before capturing the "from" frame.
    SlideStart slide()
    {
        if (m_splash_expiry_ms != 0u || m_slide_active || m_slide_pending) return SlideStart::Refused;
        const bool had_overlay = (m_overlay != Overlay::None);
        m_overlay       = Overlay::None;
        m_slide_pending = true;
        m_dirty         = true;
        return had_overlay ? SlideStart::RedrawFirst : SlideStart::Ready;
    }

    // Milliseconds from `then` to `now`, and zero when `then` has not been reached yet.
    //
    // A product's superloop samples the timestamp it draws with BEFORE the tick that may
    // open a transient, so a transient opened mid-tick is stamped a millisecond or two
    // ahead of the frame that first gets the chance to draw it. Plain unsigned subtraction
    // turns that small negative into ~4.29 billion, which is past every dwell here, and the
    // transient is retired before one frame of it reaches the glass. Reading the difference
    // as signed says "not yet", which is the truth. It is also what keeps the millisecond
    // counter's 49.7-day wrap from expiring a transient the instant it opens.
    static uint32_t since(uint32_t now, uint32_t then)
    {
        const int32_t d = (int32_t)(now - then);
        return (d > 0) ? (uint32_t)d : 0u;
    }

    // --- what this tick should do -------------------------------------------

    Decision decide(uint32_t now, bool display_busy)
    {
        Decision d;

        // A splash or a fault hold owns the screen until it expires; nothing underneath
        // is drawn while it does.
        if (m_splash_expiry_ms != 0u) {
            if (now < m_splash_expiry_ms) {
                // Animate the splash at the overlay cadence; a static hold just waits.
                if (m_splash_active && since(now, m_last_draw_ms) >= ANIM_FRAME_MS
                        && !display_busy) {
                    m_last_draw_ms = now;
                    d.what = What::SplashFrame;
                    d.arg  = since(now, m_splash_start_ms);
                }
                return d;
            }
            m_splash_expiry_ms = 0u;
            m_splash_active    = false;
            if (m_splash_after_hold) {
                m_splash_after_hold = false;
                d.what = What::SplashRestart;
                return d;
            }
            m_dirty = true;
        }

        // The transition takes precedence over the overlays and the ordinary redraw. Its
        // first tick renders the destination, and may also draw the first step.
        if (m_slide_pending) {
            m_slide_pending        = false;
            m_slide_active         = true;
            m_slide_start_ms       = now;
            d.capture_slide_target = true;
        }
        if (m_slide_active) {
            const uint32_t elapsed = since(now, m_slide_start_ms);
            if (elapsed < SLIDE_MS && since(now, m_last_draw_ms) < ANIM_FRAME_MS) return d;
            if (display_busy) return d;
            m_last_draw_ms = now;
            if (elapsed >= SLIDE_MS) {
                m_slide_active = false;
                m_dirty        = true;
                d.what         = What::SlideSettle;
            } else {
                d.what = What::SlideStep;
                d.arg  = elapsed;
            }
            return d;
        }

        // An expired transient hands the screen back, which is itself a change to draw.
        bool animating = false;
        uint32_t overlay_elapsed = 0u;
        if (m_overlay != Overlay::None) {
            overlay_elapsed = since(now, m_overlay_start_ms);
            if (overlay_elapsed >= overlay_total()) {
                m_overlay = Overlay::None;
                m_dirty   = true;
            } else {
                animating = overlay_animating(overlay_elapsed);
            }
        }

        // Redraw on state change, or every animation frame while an overlay animates;
        // otherwise idle at the 20 fps cap. Input is polled elsewhere, so a redraw never
        // delays a knob or footswitch.
        const uint32_t cap = animating ? ANIM_FRAME_MS : REFRESH_MS;
        if (!m_dirty && since(now, m_last_draw_ms) < cap) return d;
        if (display_busy) return d;

        m_dirty        = false;
        m_last_draw_ms = now;
        switch (m_overlay) {
            case Overlay::Panel:  d.what = What::FramePanel;  d.arg = overlay_prog(overlay_elapsed); break;
            case Overlay::Banner: d.what = What::FrameBanner; d.arg = overlay_prog(overlay_elapsed); break;
            case Overlay::Save:   d.what = What::FrameSave;                                          break;
            default:              d.what = What::Frame;                                              break;
        }
        return d;
    }

private:
    // Pacing.
    static constexpr uint32_t REFRESH_MS     = 50u;
    static constexpr uint32_t ANIM_FRAME_MS  = 16u;
    static constexpr uint32_t ANIM_MS        = 140u;
    static constexpr uint32_t SPLASH_MS      = 2000u;
    static constexpr uint32_t FAULT_HOLD_MS  = 2500u;

    // How long a transient stays up before it hands the screen back. One dwell for all
    // three: the save confirmation is a thing to read, the same as the panel and the banner
    // are, so it is held for the same time the product gives anything else to be read.
    uint32_t overlay_total() const { return (uint32_t)DISPLAY_PARAM_SHOW_MS; }

    // Enter/exit progress, 0..256, for the timed panel and banner.
    uint16_t overlay_prog(uint32_t elapsed) const
    {
        if (elapsed < ANIM_MS) return (uint16_t)(elapsed * 256u / ANIM_MS);
        if (elapsed < DISPLAY_PARAM_SHOW_MS - ANIM_MS) return 256u;
        if (elapsed < DISPLAY_PARAM_SHOW_MS)
            return (uint16_t)((DISPLAY_PARAM_SHOW_MS - elapsed) * 256u / ANIM_MS);
        return 0u;
    }

    // Whether the overlay is mid-animation this tick, which drives the faster cadence. The
    // save confirmation is a still frame, so it never asks for one.
    bool overlay_animating(uint32_t elapsed) const
    {
        if (m_overlay == Overlay::Save) return false;
        return elapsed < ANIM_MS || elapsed >= DISPLAY_PARAM_SHOW_MS - ANIM_MS;
    }

    bool     m_dirty         = true;
    uint32_t m_last_draw_ms  = 0u;

    Overlay  m_overlay          = Overlay::None;
    uint32_t m_overlay_start_ms = 0u;

    uint32_t m_splash_expiry_ms  = 0u;
    uint32_t m_splash_start_ms   = 0u;
    bool     m_splash_active     = false;
    bool     m_splash_after_hold = false;

    bool     m_slide_pending  = false;
    bool     m_slide_active   = false;
    uint32_t m_slide_start_ms = 0u;
};

}  // namespace pedal_core::ui
