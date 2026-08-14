#pragma once
#include <cstdint>

// Host-portable quadrature decode logic, factored out of encoder.cpp so the
// correctness-critical decode table can be unit-tested without the CMSIS/EXTI/GPIO
// machinery the rest of the driver needs. A wrong table entry would make the
// encoder skip detents or count the wrong direction, so it is worth locking down.
//
// State encoding: bit1 = A, bit0 = B (each pin 0/1). Each detent is four
// quarter-steps; read_delta() divides the accumulated quarter-steps by 4.
namespace encoder_decode {

    // Indexed by (prev_state << 2 | curr_state).
    // CW  full cycle: 00 -> 10 -> 11 -> 01 -> 00  (+4 quarter-steps per detent)
    // CCW full cycle: 00 -> 01 -> 11 -> 10 -> 00  (-4 quarter-steps per detent)
    inline constexpr int8_t k_qtable[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    // Quarter-step delta for a transition from prev to curr (each 0..3).
    inline int8_t step(uint8_t prev, uint8_t curr) {
        return k_qtable[((prev & 0x3u) << 2) | (curr & 0x3u)];
    }
}
