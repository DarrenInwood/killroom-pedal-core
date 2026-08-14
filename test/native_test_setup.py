"""
PlatformIO extra script: compile test/support/ files for the native test env.

PlatformIO's test runner does not automatically recurse into support/
subdirectories when collecting shared test code, so we explicitly build the
stubs and fakes here.

They are built into a STATIC LIBRARY (not linked as plain objects) so a test
that #includes a real driver/module .cpp can supply its own definitions of the
stubbed symbols: the linker then pulls only the library members a given test
leaves undefined. Force-linking the objects instead would make those test-local
definitions collide with the fakes (multiple-definition errors) — which is why,
e.g., test_eeprom (real driver) can coexist with eeprom_fake (used by
test_presets) only as a library.
"""
Import("env")

lib = env.BuildLibrary(
    "$BUILD_DIR/test_support",
    env.subst("$PROJECT_TEST_DIR") + "/support"
)
env.Append(LIBS=[lib])
