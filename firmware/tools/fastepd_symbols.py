"""PlatformIO extra script (env reader_t547): renames the symbols that FastEPD
and bb_epaper both define, inside FastEPD only. Both libraries are by BitBank and
share helper code (pixel setters, UTF-8 to codepage 1252, the Group5 codec) with
identical C++ signatures, so linking both fails with "multiple definition".
The renamed copies are FastEPD-internal; firmware code never calls them.
"""

Import("env")  # noqa: F821 (SCons)

SHARED = [
    "RotateCharBox",
    "bbepSetPixel2Clr",
    "bbepSetPixel4Clr",
    "bbepSetPixel16Clr",
    "bbepSetPixelFast2Clr",
    "bbepSetPixelFast4Clr",
    "bbepSetPixelFast16Clr",
    "bbepStretchAndSmooth",
    "bbepUnicodeString",
    "bbepUnicodeTo1252",
    "G5DECODER",
    "G5ENCODER",
]

for lib in env.GetLibBuilders():  # noqa: F821
    if lib.name == "FastEPD":
        lib.env.Append(CPPDEFINES=[(name, "fastepd_" + name) for name in SHARED])
