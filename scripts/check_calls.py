"""PlatformIO post-link check: every call in src/'s code lands on a function.

scripts/lto.py has the linker shrink src/'s code after it's laid out (a long call whose target
turned out to be in reach becomes a direct one, and the freed instruction goes). The linker then
moves every symbol and call target that sits after a removed byte, and GNU ld's Xtensa code for
that has at least one bug: a symbol an object reaches under two names is moved twice. That's
what `--wrap=X` makes of an object that defines __wrap_X and also calls X; a __wrap_log_printf
in src/ (2026-11-24) ended up before its own code, and every log line would have jumped into
the middle of another function. Nothing fails at link time unless the wrong address happens to
be misaligned ("dangerous relocation: call8: misaligned call target"). The cause as read from
binutils' behaviour, not its source: the diagnosis is the symptom plus what made it go away.

So after each link this reads firmware.elf and checks what such a bug breaks. Every C and C++
function here begins with an `entry` instruction (the windowed ABI), so:
  - every call in src/'s code (the LTO partitions, found in the map) must land on one: a direct
    call (call8) by its offset, a long one (l32r of the address, then callx8) by the literal it
    loads, if that's an address in flash or IRAM code (the ROM's functions aren't in the image
    to look at);
  - so must every function a vtable points at (the image's virtual calls: each screen's
    draw() and onKey() among them).
A miss fails the build and says where. It takes a fraction of a second.

It can also be run by hand: python3 scripts/check_calls.py firmware.elf firmware.map
"""

import re
import struct
import sys

TEXT_SECTIONS = (".flash.text", ".iram0.text")
# In the map: one input section of an LTO partition, e.g.
#  .text          0x42001304    0x1bde8 /tmp/ccXXXX.ltrans1.ltrans.o
PARTITION_RE = re.compile(r"^ \.text\S*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S*\.ltrans\.o$")


class Image:
    """The parts of an ELF32 little-endian file this needs: section contents and functions."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:6] != b"\x7fELF\x01\x01":
            raise ValueError("%s: not a 32-bit little-endian ELF file" % path)
        shoff, = struct.unpack_from("<I", self.data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", self.data, 0x2E)
        headers = [struct.unpack_from("<10I", self.data, shoff + i * shentsize) for i in range(shnum)]
        names = headers[shstrndx][4]
        self.sections = {}
        for name, _type, _flags, addr, offset, size, link, _info, _align, entsize in headers:
            end = self.data.index(b"\0", names + name)
            self.sections[self.data[names + name : end].decode()] = (addr, offset, size, link, entsize)
        self.text = [
            (addr, self.data[offset : offset + size])
            for addr, offset, size, _link, _entsize in (self.sections[s] for s in TEXT_SECTIONS if s in self.sections)
        ]
        self.functions = []  # (address, size, name), by address
        self.vtables = []  # the same for the C++ vtables (_ZTV...)
        _addr, offset, size, link, entsize = self.sections[".symtab"]
        strings = headers[link][4]
        for i in range(0, size, entsize or 16):
            name, value, sym_size, info, _other, _shndx = struct.unpack_from("<IIIBBH", self.data, offset + i)
            if not sym_size or info & 0xF not in (1, 2):  # STT_OBJECT, STT_FUNC
                continue
            end = self.data.index(b"\0", strings + name)
            text = self.data[strings + name : end].decode()
            if info & 0xF == 2:
                self.functions.append((value, sym_size, text))
            elif text.startswith("_ZTV"):
                self.vtables.append((value, sym_size, text))
        self.functions.sort()

    def read(self, address, count):
        for start, content in self.text:
            if start <= address and address + count <= start + len(content):
                return content[address - start : address - start + count]
        return None

    def read_data(self, address, count):
        """Bytes from any section that's in the file (a vtable is in the read-only data)."""
        for name, (start, offset, size, _link, _entsize) in self.sections.items():
            if start and start <= address and address + count <= start + size and not name.startswith(".debug"):
                return self.data[offset + address - start : offset + address - start + count]
        return None

    def is_entry(self, address):
        code = self.read(address, 2)
        return code is not None and code[0] == 0x36 and code[1] & 0x0F == 1  # entry a1, N


def calls_in(image, start, size):
    """The calls in a function whose target the image tells, as (address, target, how): call8,
    and callx8 through a register an l32r loaded in the instruction before. Xtensa instructions
    here are 2 or 3 bytes by their first byte's low nibble; after a jump or a return, zero bytes
    are padding before the next block."""
    code = image.read(start, size)
    i = 0
    loaded = None  # (register, literal's address) if the instruction before was an l32r
    while code and i < len(code):
        first = code[i]
        low = first & 0x0F
        length = 4 if low >= 0xE else 2 if low >= 0x8 else 3
        if i + length > len(code):
            break
        ends_block = False
        was_loaded, loaded = loaded, None
        if length == 3:
            second, third = code[i + 1], code[i + 2]
            address = start + i
            if first & 0x3F == 0x25:  # call8
                offset = (third << 10) | (second << 2) | (first >> 6)
                if offset & 0x20000:
                    offset -= 0x40000
                yield address, ((address & ~3) + 4 + (offset << 2)) & 0xFFFFFFFF, "call8"
            elif low == 0x1:  # l32r: the literal is up to 256 KB back, word-aligned
                back = 0x10000 - ((third << 8) | second)
                loaded = (first >> 4, ((address + 3) & ~3) - (back << 2))
            elif first == 0xE0 and third == 0 and was_loaded and was_loaded[0] == second:  # callx8
                word = image.read(was_loaded[1], 4)
                if word is not None:
                    yield address, struct.unpack("<I", word)[0], "callx8"
            ends_block = (
                first & 0x3F == 0x06  # j
                or (first, second, third) in ((0x80, 0, 0), (0x90, 0, 0))  # ret, retw
                or (first == 0xA0 and third == 0 and second & 0xF0 == 0)  # jx
            )
        elif length == 2:
            ends_block = code[i : i + 2] in (b"\x1d\xf0", b"\x0d\xf0")  # retw.n, ret.n
        i += length
        if ends_block:
            while i < len(code) and code[i] == 0:
                i += 1


def partitions(map_path):
    """src/'s code in the image: the LTO partitions' text, as (start, end) ranges."""
    text = []
    with open(map_path, errors="replace") as f:
        for line in f:
            if ".ltrans.o" not in line:
                continue
            m = PARTITION_RE.match(line.rstrip("\n"))
            if m and int(m.group(2), 16):
                start = int(m.group(1), 16)
                text.append((start, start + int(m.group(2), 16)))
    return text


def check(elf_path, map_path):
    """The problems found, as lines of text, and how much was checked."""
    image = Image(elf_path)
    text = partitions(map_path)
    problems = []
    direct = long_calls = 0
    for start, size, name in image.functions:
        if not any(a <= start < b for a, b in text) or not image.is_entry(start):
            continue
        for address, target, how in calls_in(image, start, size):
            if image.read(target, 1) is None:
                continue  # a long call out of the image: the ROM
            if how == "call8":
                direct += 1
            else:
                long_calls += 1
            if not image.is_entry(target):
                problems.append("0x%08x in %s: %s 0x%08x isn't a function's entry" % (address, name, how, target))
    virtual = 0
    for start, size, name in image.vtables:
        table = image.read_data(start, size) or b""
        for i in range(0, len(table) - 3, 4):
            target, = struct.unpack_from("<I", table, i)
            if image.read(target, 1) is None:
                continue  # the offset to the top, the type info, or nothing
            virtual += 1
            if not image.is_entry(target):
                problems.append("%s + %d: 0x%08x isn't a function's entry" % (name, i, target))
    return problems, direct, long_calls, virtual, len(text)


def report(elf_path, map_path):
    try:
        problems, direct, long_calls, virtual, count = check(elf_path, map_path)
    except (OSError, ValueError, KeyError, struct.error) as e:
        # Unchecked is not the same as fine: say so, and stop.
        print("check_calls: can't read %s or its map (%s: %s)" % (elf_path, type(e).__name__, e))
        return 1
    if not count:
        print("check_calls: no LTO partitions in %s: nothing checked" % map_path)
        return 0
    if problems:
        print("check_calls: %d calls don't land on a function:" % len(problems))
        for line in problems[:20]:
            print("  " + line)
        print("  (the linker moved a symbol wrongly: see scripts/check_calls.py)")
        return 1
    print("check_calls: src/'s %d direct and %d long calls and the vtables' %d functions all land on functions"
          % (direct, long_calls, virtual))
    return 0


if "Import" in globals():  # as a PlatformIO extra script
    Import("env")  # noqa: F821 - SCons injects Import into extra scripts

    def after_link(source, target, env):  # noqa: ARG001 - SCons's signature
        elf = str(target[0])
        if report(elf, elf[: -len(".elf")] + ".map"):
            env.Exit(1)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", after_link)  # noqa: F821
elif __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(report(sys.argv[1], sys.argv[2]))
