#pragma once

// Which optional domains of the library this product wants.
//
// Reference copy for the library's own native tests; a consuming product provides its real
// pedal_core_features.hpp on the include path ahead of test/support.
//
// A product's own copy simply defines each switch outright. This one defers to the build,
// because the library has to compile BOTH sides of every switch: `native` takes the
// defaults below, and `native_minimal` passes -D to turn them off. A second stub file of
// the same name would not do -- a quoted include resolves against the including file's own
// directory first, so which of the two a translation unit got would depend on where it sat.
//
// A switch left undefined is not defaulted anywhere outside this file: the modules check
// for it and stop the build, because a preprocessor zero would compile a domain to nothing
// and lose its symbols somewhere the linker reports as an unrelated failure.
//
// A domain switched on obliges the product to supply that domain's config header too --
// pedal_core_tempo_config.hpp for the tempo layer, pedal_core_extinput_config.hpp for the
// external jack. The reference stubs beside this file name every constant each needs.

// The tempo layer: tap_tempo, tempo_controller, tempo_led and the generated MIDI clock.
#ifndef PEDAL_CORE_HAS_TEMPO
#  define PEDAL_CORE_HAS_TEMPO 1
#endif

// The external-input jack and its assignments.
#ifndef PEDAL_CORE_HAS_EXTINPUT
#  define PEDAL_CORE_HAS_EXTINPUT 1
#endif
