"""PlatformIO post-build-setup hook: link-time optimization for the firmware's own code.

platformio.ini compiles src/ with `build_src_flags = -flto`, so those objects carry GCC's
intermediate code. LTO happens when the linker gets -flto as well, but the framework's link flags
(flags/ld_flags in the esp32s3 libs package) end with -fno-lto, which turns it off again, and
build_unflags can't remove it. This script swaps it for -flto=auto. LINKFLAGS nests lists inside
lists, so it walks them. Saves about 20 KB in firmware.bin.

Only src/ is built that way. The Arduino core and the libraries stay plain objects: LTO across
the whole build fails to link (the core's __wrap_esp_panic_handler ends in a "dangerous
relocation"), and adding libsmb2 saves under 3 KB more.
"""

from collections import UserList

Import("env")  # noqa: F821 - SCons injects Import into extra scripts

SEQUENCES = (list, tuple, UserList)  # UserList: SCons's CLVar


def swap(flags):
    if isinstance(flags, SEQUENCES):
        return type(flags)([swap(f) for f in flags])
    return "-flto=auto" if flags == "-fno-lto" else flags  # =auto: LTRANS jobs in parallel


def flat(flags):
    if isinstance(flags, SEQUENCES):
        return [x for f in flags for x in flat(f)]
    return [flags]


env.Replace(LINKFLAGS=swap(env["LINKFLAGS"]))  # noqa: F821
if "-flto=auto" not in flat(env["LINKFLAGS"]):  # noqa: F821
    raise SystemExit("scripts/lto.py: no -fno-lto in LINKFLAGS to replace; check the framework's ld_flags")
