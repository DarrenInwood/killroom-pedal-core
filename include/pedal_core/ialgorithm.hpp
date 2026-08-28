#pragma once
#include <cstdint>

// The algorithm surface the shared UI layer reads. A product's algorithm base
// (a DSP effect with routing and clocks, a stateless algorithm over a parameter
// vector — whatever it is) implements these ten and the shared code needs
// nothing else from it.
//
// There is deliberately no set_param here. Every parameter write funnels
// through the product's pedal, which owns the write path — zone snapping,
// settle latching, clamping and their side effects differ per product and
// stay there. The shared layer reads values and renders them via
// param_display; reset() is the one mutation, "restore the declared
// defaults", whose meaning every product already defines.
//
// The tempo group is for products with a tempo layer; the defaults mean "no
// tempo behaviour" and a product without one leaves them alone.
namespace pedal_core {

class IAlgorithm {
public:
    virtual const char* name() const = 0;
    virtual uint8_t     num_params() const = 0;
    virtual uint16_t    get_param(uint8_t idx) const = 0;
    virtual const char* param_name(uint8_t idx) const = 0;
    virtual void        param_display(uint8_t idx, uint16_t value,
                                      char* buf, uint8_t buf_len) const = 0;
    virtual uint16_t    default_param(uint8_t idx) const = 0;
    virtual void        reset() = 0;

    virtual int8_t tempo_param() const { return -1; }   // -1 = no tempo parameter
    virtual void   set_tempo(float) {}
    virtual float  tempo_bpm() const { return 0.0f; }

    // The rate the tempo LED locks to, in whole milliseconds, or 0 where the
    // algorithm has no beat to show. This is the algorithm's primary musical
    // rate rather than the tempo itself: a tremolo running in 3/8 flashes at
    // 3/8 of a beat, which is the thing a player watching the LED reads. 0 is
    // the same statement tempo_bpm() makes by answering 0 — no beat, rather
    // than a beat of zero.
    virtual uint32_t beat_period_ms() const { return 0u; }

protected:
    ~IAlgorithm() = default;   // never deleted through the interface
};

}  // namespace pedal_core
