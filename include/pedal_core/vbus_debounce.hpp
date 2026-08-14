#pragma once
#include <cstdint>

// Debounced edge detector for the USB VBUS presence line (PC9). Kept dependency-free — no
// TinyUSB, no CMSIS — so the state machine is host-testable, the same split the CIN table
// (usb_midi_cin.hpp) uses. The usb_midi driver owns the GPIO read, the millisecond clock, and
// the tud_connect()/tud_disconnect() calls this decides; here lives only the logic that turns a
// noisy raw sample into a clean connect/disconnect edge.
namespace vbus {

// The action a settled sample calls for. None while the line is steady or still bouncing.
enum class Edge : uint8_t { None, Connected, Disconnected };

class Debouncer {
public:
    // Adopt a known starting state — the cable level sampled at init — without emitting an edge.
    void seed(bool present, uint32_t now_ms)
    {
        m_present   = present;
        m_candidate = present;
        m_edge_ms   = now_ms;
    }

    // Feed one poll. Returns Connected/Disconnected once a new level has held steady for
    // debounce_ms, so connector bounce on plug/unplug can't flap the pull-up; None otherwise.
    Edge update(bool raw, uint32_t now_ms, uint32_t debounce_ms)
    {
        if (raw != m_candidate) {
            m_candidate = raw;        // level changed — restart the settle timer
            m_edge_ms   = now_ms;
        } else if (raw != m_present && (now_ms - m_edge_ms) >= debounce_ms) {
            m_present = raw;          // steady long enough — commit and report the edge
            return raw ? Edge::Connected : Edge::Disconnected;
        }
        return Edge::None;
    }

    // The committed (debounced) state — mirrors what the D+ pull-up is doing.
    bool present() const { return m_present; }

private:
    bool     m_present   = false;  // last committed level
    bool     m_candidate = false;  // level currently settling
    uint32_t m_edge_ms   = 0;      // time the candidate first appeared
};

} // namespace vbus
