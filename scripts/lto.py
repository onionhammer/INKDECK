"""PlatformIO post-build-setup hook: link-time optimization for the firmware's own code.

platformio.ini compiles src/ with `build_src_flags = -flto`, so those objects carry GCC's
intermediate code. LTO happens when the linker gets -flto as well, but the framework's link flags
(flags/ld_flags in the esp32s3 libs package) end with -fno-lto, which turns it off again, and
build_unflags can't remove it. This script swaps it for -flto=auto. LINKFLAGS nests lists inside
lists, so it walks them. Saves about 50 KB in firmware.bin.

Only src/ is built that way. The Arduino core and the libraries stay plain objects: LTO across
the whole build fails to link (the core's __wrap_esp_panic_handler ends in a "dangerous
relocation"), and adding libsmb2 saves under 3 KB more.

LTO generates src/'s code at link time, from the link command's flags, so the script adds four
that decide how that code comes out (firmware.bin, each on top of the ones before, measured
2026-11-24; 90.5 KB together):

  -gdwarf-4 -ggdb   50.0 KB. The framework compiles everything with these, but they aren't on
      the link command, so LTO's code generation got plain -g: DWARF 5. For DWARF 5 GCC writes
      address ranges as differences between labels, and the assembler then marks the code "no
      transform". The linker shortens a long call (load the address, call through the register)
      to a direct one when the target turns out to be in reach; in code it may not transform it
      can't take the load out, and leaves a 3-byte NOP in its place: 15,900 of them in src/.
      With DWARF 4, like the rest of the build, they go.
  -Wl,--size-opt   17.9 KB. The Xtensa linker's switch for that shortening: without it the
      linker pads again to keep branch targets aligned.
  --param=lto-partitions=6   2.9 KB (19.8 KB before the two above). LTO compiles src/ as
      partitions, 27 by default at this size. Calls and constants are cheaper inside one (a
      direct call, one shared pool of literals), so fewer, larger ones come out smaller. The
      limit: an instruction reaches its literals 256 KB back, and src/ as a single partition
      fails to link ("l32r: literal target out of range"). Six are 100 to 140 KB each today;
      past about 1.5 MB of src/ code, raise the count (and see flash_text_order.ld).
  -Wl,--section-ordering-file   19.9 KB. scripts/flash_text_order.ld puts the two partitions
      that everything calls in the middle of src/'s code, where all of it reaches them.

Tried and left out (same day): -ffunction-sections -fdata-sections for LTO's output (16 KB
larger: every function gets its own literal pool), -flto-partition=one with -mauto-litpools
(7 KB larger), -Oz (the same code as -Os on Xtensa), -fno-partial-inlining,
-fno-inline-small-functions, -fno-ipa-sra, -fno-ivopts, -fjump-tables (each a little larger).
"""

import os
from collections import UserList

Import("env")  # noqa: F821 - SCons injects Import into extra scripts

SEQUENCES = (list, tuple, UserList)  # UserList: SCons's CLVar

LTO_PARTITIONS = 6  # flash_text_order.ld names the last one (5)


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

text_order = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "flash_text_order.ld")  # noqa: F821
if not os.path.isfile(text_order):
    raise SystemExit("scripts/lto.py: %s is missing" % text_order)
env.Append(  # noqa: F821
    LINKFLAGS=[
        "-gdwarf-4",
        "-ggdb",
        "-Wl,--size-opt",
        "--param=lto-partitions=%d" % LTO_PARTITIONS,
        "-Wl,--section-ordering-file=" + text_order,
    ]
)
