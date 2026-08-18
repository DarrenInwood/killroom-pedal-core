// The empty default for midi_handler's one weak callback.
//
// on_midi_note_off is the seventh MIDI callback and the only optional one: a product
// that tracks note lifetimes defines its own and this is overridden at link time, while
// a product with no use for notes links without naming it. The other six stay mandatory,
// so nothing here changes what an existing product has to supply.
//
// It lives in its own translation unit rather than beside the dispatcher because
// test_midi_handler compiles midi_handler.cpp directly into the test's TU and defines
// the callback there to observe it; a definition in that file would collide.
#include <cstdint>

extern "C" __attribute__((weak)) void on_midi_note_off(uint8_t channel, uint8_t note,
                                                       uint8_t velocity)
{
    (void)channel; (void)note; (void)velocity;
}
