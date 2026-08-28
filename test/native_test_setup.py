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

import os

# Where the image-descriptor fixtures live, handed to the C++ suite as a macro.
#
# The path is normalised to forward slashes before it becomes a string literal: on a
# Windows checkout $PROJECT_DIR is backslashed, and "C:\Users\..." in a C string is a
# run of invalid escape sequences rather than a path.
_fixtures = os.path.join(env.subst("$PROJECT_DIR"), "test", "fixtures", "app_image")
env.Append(CPPDEFINES=[("APP_IMAGE_FIXTURE_DIR", env.StringifyMacro(_fixtures.replace(os.sep, "/")))])

lib = env.BuildLibrary(
    "$BUILD_DIR/test_support",
    env.subst("$PROJECT_TEST_DIR") + "/support"
)
env.Append(LIBS=[lib])
