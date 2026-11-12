# /// script
# requires-python = ">=3.10"
# dependencies = ["freetype-py>=2.4"]
# ///
"""Generates the firmware's bitmap fonts (Adafruit GFX format).

    uv run tools/gen_fonts.py

Fonts come from pinned, sha256-checked downloads cached in .cache/fonts/:
  - JetBrains Mono 2.304 (UI), from the JetBrains release zip;
  - Literata (book text), the variable font from google/fonts.
Each face/size in FONTS is rasterised with FreeType's hinted monochrome renderer. Output goes
to src/ui/fonts/: glyphs.h (the slot scheme), jbmono.{h,cpp}, literata.{h,cpp}, and the
licences. Edit FONTS and re-run to add a size.

Glyph slots: each font covers bytes 0x20..0xFF (or a subset, see CHARSETS).
  0x20-0x7E  ASCII
  0x80-0x9F  UI and typographic symbols from EXTRAS (that C1 range is unused in Latin-1)
  0xA0-0xFF  Latin-1 Supplement, same as the code point
Firmware text is either UTF-8 (UI strings, mapped with ui::fonts::slotFor) or already in slots
(EPUB flow text, see text.h).
"""

from __future__ import annotations

import hashlib
import io
import pathlib
import sys
import urllib.request
import zipfile

import freetype

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache" / "fonts"
OUT = ROOT / "src" / "ui" / "fonts"

LITERATA_BASE = (
    "https://raw.githubusercontent.com/google/fonts/4e5f06dbb274a27ebe71ed54ea706b3ee40eabd9/ofl/literata/"
)
FAMILIES = {
    "jbmono": {
        "label": "JetBrains Mono 2.304",
        "zip": (
            "https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip",
            "6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf",
        ),
        "faces": {  # CSS weight -> file in the zip
            500: "fonts/ttf/JetBrainsMono-Medium.ttf",
            700: "fonts/ttf/JetBrainsMono-Bold.ttf",
            800: "fonts/ttf/JetBrainsMono-ExtraBold.ttf",
        },
        "license": ("OFL.txt", "OFL-JetBrainsMono.txt"),
    },
    "literata": {
        "label": "Literata, google/fonts 4e5f06d",
        "files": {
            "roman": (LITERATA_BASE + "Literata%5Bopsz,wght%5D.ttf",
                      "b41138c9373112f32abb589cc22e8674b06ed4048b0c513be922bdd26f274440"),
            "italic": (LITERATA_BASE + "Literata-Italic%5Bopsz,wght%5D.ttf",
                       "d483dfaeba9cbf4ce71d32a52ee65df82f7e35b15fff8d1011cdb242d1fcd465"),
            "license": (LITERATA_BASE + "OFL.txt",
                        "8742963604cd89dc81437811a850018fc03b2bfad686d7422c8235967c87614e"),
        },
        "license": ("license", "OFL-Literata.txt"),
    },
}

# Slot 0x80 + i holds EXTRAS[i]. Append only: reordering changes every font's slot numbers.
EXTRAS = [
    0x25B2,  # ▲
    0x25BC,  # ▼
    0x25B6,  # ▶
    0x25B8,  # ▸
    0x25CF,  # ●
    0x25CB,  # ○
    0x2713,  # ✓
    0x2212,  # − minus
    0x2013,  # – en dash
    0x2014,  # — em dash
    0x2026,  # … ellipsis
    0x2018,  # ‘
    0x2019,  # ’
    0x201C,  # “
    0x201D,  # ”
    0x2190,  # ←
    0x2191,  # ↑
    0x2192,  # →
    0x2193,  # ↓
    0x25A0,  # ■
    0x25A1,  # □
    0x2022,  # • bullet
]
assert len(EXTRAS) <= 32

CHARSETS = {
    "latin": (0x20, 0xFF),  # ASCII + extras + Latin-1
    "upper": (0x20, 0xDE),  # the same without lower-case letters (big titles are upper-cased)
    "ascii": (0x20, 0x7E),
    "caps": (0x30, 0x5A),  # digits, a little punctuation, A-Z (drop caps)
    "digits": (0x30, 0x3A),  # 0-9 and ':'
}


def in_charset(charset: str, slot: int) -> bool:
    """Whether a slot inside the charset's range gets a glyph (others stay empty)."""
    if charset in ("latin", "upper"):
        return not (charset == "upper" and ord("a") <= slot <= ord("z"))
    return slot <= 0x7E

# C++ name, family, pixel size (= CSS font-size), charset, and the face:
#   jbmono:   {"weight": 500|700|800}
#   literata: {"italic": bool, "wght": 200..900}; the optical size follows the pixel size
# Sizes and weights follow the design canvas (docs/ui-design.md). ui/type.h and fonts.cpp map
# them to roles.
FONTS = [
    ("kMono12Bold", "jbmono", 12, "latin", {"weight": 700}),
    ("kMono13Medium", "jbmono", 13, "latin", {"weight": 500}),
    ("kMono13Bold", "jbmono", 13, "latin", {"weight": 700}),
    ("kMono13ExtraBold", "jbmono", 13, "latin", {"weight": 800}),
    ("kMono14Medium", "jbmono", 14, "latin", {"weight": 500}),
    ("kMono14Bold", "jbmono", 14, "latin", {"weight": 700}),
    ("kMono14ExtraBold", "jbmono", 14, "latin", {"weight": 800}),
    ("kMono15Medium", "jbmono", 15, "latin", {"weight": 500}),
    ("kMono15Bold", "jbmono", 15, "latin", {"weight": 700}),
    ("kMono15ExtraBold", "jbmono", 15, "latin", {"weight": 800}),
    ("kMono16Bold", "jbmono", 16, "latin", {"weight": 700}),
    ("kMono17Bold", "jbmono", 17, "latin", {"weight": 700}),
    ("kMono17ExtraBold", "jbmono", 17, "latin", {"weight": 800}),
    ("kMono18Medium", "jbmono", 18, "latin", {"weight": 500}),
    ("kMono18Bold", "jbmono", 18, "latin", {"weight": 700}),
    ("kMono38ExtraBold", "jbmono", 38, "upper", {"weight": 800}),
    ("kMono40ExtraBold", "jbmono", 40, "digits", {"weight": 800}),
    ("kMono50ExtraBold", "jbmono", 50, "upper", {"weight": 800}),
    ("kMono68ExtraBold", "jbmono", 68, "caps", {"weight": 800}),
    ("kMono104ExtraBold", "jbmono", 104, "digits", {"weight": 800}),
]
# Reader text: three sizes (19 px is the spec's), each regular / bold / italic / bold italic.
# Bold is 600, the weight the design canvas loads.
for _px in (16, 19, 23):
    FONTS += [
        (f"kLiterata{_px}", "literata", _px, "latin", {"italic": False, "wght": 400}),
        (f"kLiterata{_px}Bold", "literata", _px, "latin", {"italic": False, "wght": 600}),
        (f"kLiterata{_px}Italic", "literata", _px, "latin", {"italic": True, "wght": 400}),
        (f"kLiterata{_px}BoldItalic", "literata", _px, "latin", {"italic": True, "wght": 600}),
    ]


def slot_codepoint(slot: int) -> int | None:
    if 0x80 <= slot <= 0x9F:
        i = slot - 0x80
        return EXTRAS[i] if i < len(EXTRAS) else None
    return slot


def download(url: str, sha256: str) -> bytes:
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / pathlib.Path(urllib.request.url2pathname(url.rsplit("/", 1)[1])).name
    if not path.exists():
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as r:
            path.write_bytes(r.read())
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != sha256:
        sys.exit(f"{url}: sha256 {digest} != pinned {sha256}")
    return data


class Sources:
    """Font files and licences, by family."""

    def __init__(self) -> None:
        self.jb_zip = zipfile.ZipFile(io.BytesIO(download(*FAMILIES["jbmono"]["zip"])))
        self.literata = {k: download(*v) for k, v in FAMILIES["literata"]["files"].items()}

    def face(self, family: str, opts: dict, px: int) -> freetype.Face:
        if family == "jbmono":
            data = self.jb_zip.read(FAMILIES["jbmono"]["faces"][opts["weight"]])
            face = freetype.Face(io.BytesIO(data))
            face.set_pixel_sizes(0, px)
        else:
            face = freetype.Face(io.BytesIO(self.literata["italic" if opts["italic"] else "roman"]))
            face.set_pixel_sizes(0, px)
            face.set_var_design_coords((float(px), float(opts["wght"])))  # axes: opsz, wght
        return face

    def license(self, family: str) -> bytes:
        src = FAMILIES[family]["license"][0]
        return self.jb_zip.read(src) if family == "jbmono" else self.literata[src]


def rasterise(face: freetype.Face, cp: int | None):
    """Returns (rows of 0/1, width, height, xAdvance, xOffset, yOffset) or None."""
    if cp is None or face.get_char_index(cp) == 0:
        return None
    face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    g = face.glyph
    bm = g.bitmap
    rows = []
    for y in range(bm.rows):
        row = bm.buffer[y * bm.pitch : (y + 1) * bm.pitch]
        rows.append([(row[x >> 3] >> (7 - (x & 7))) & 1 for x in range(bm.width)])
    advance = (g.advance.x + 32) >> 6
    return rows, bm.width, bm.rows, advance, g.bitmap_left, -g.bitmap_top


def build_font(name: str, face: freetype.Face, charset: str):
    first, last = CHARSETS[charset]
    bitmap = bytearray()
    glyphs = []
    for slot in range(first, last + 1):
        g = rasterise(face, slot_codepoint(slot)) if in_charset(charset, slot) else None
        if g is None:
            glyphs.append((len(bitmap), 0, 0, 0, 0, 0, slot))
            continue
        rows, w, h, adv, xo, yo = g
        assert w < 256 and h < 256 and adv < 256 and -128 <= yo < 128, (name, slot)
        offset = len(bitmap)
        acc = nbits = 0
        for row in rows:
            for bit in row:
                acc = (acc << 1) | bit
                nbits += 1
                if nbits == 8:
                    bitmap.append(acc)
                    acc = nbits = 0
        if nbits:
            bitmap.append(acc << (8 - nbits))
        glyphs.append((offset, w, h, adv, xo, yo, slot))
    assert len(bitmap) < 65536, name
    return first, last, face.size.height >> 6, bitmap, glyphs


def c_char_comment(slot: int) -> str:
    cp = slot_codepoint(slot)
    if cp is None:
        return "unused"
    return f"U+{cp:04X} {chr(cp)}" if cp > 0x20 and chr(cp) != "\\" else f"U+{cp:04X}"


def write(path: pathlib.Path, lines: list[str]) -> None:
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def banner(what: str) -> list[str]:
    return [
        f"// GENERATED by tools/gen_fonts.py ({what}).",
        "// Do not edit: change the script and re-run it.",
    ]


def write_glyphs_header() -> None:
    lines = banner("glyph slot scheme") + [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace ui::fonts {",
        "",
        "// Every generated font indexes glyphs by slot: ASCII 0x20-0x7E, kExtraCodepoints[i] at",
        "// 0x80 + i, Latin-1 0xA0-0xFF.",
        "constexpr uint8_t kExtraSlotBase = 0x80;",
        f"constexpr int kExtraCount = {len(EXTRAS)};",
        "constexpr uint32_t kExtraCodepoints[kExtraCount] = {",
    ]
    lines += [f"    0x{cp:04X},  // {chr(cp)}" for cp in EXTRAS]
    lines += [
        "};",
        "",
        "// The slot for a code point, or 0 if the fonts have none (transliterate it instead).",
        "inline uint8_t slotFor(uint32_t cp) {",
        "  if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF)) {",
        "    return static_cast<uint8_t>(cp);",
        "  }",
        "  for (int i = 0; i < kExtraCount; ++i) {",
        "    if (kExtraCodepoints[i] == cp) return static_cast<uint8_t>(kExtraSlotBase + i);",
        "  }",
        "  return 0;",
        "}",
        "",
        "}  // namespace ui::fonts",
    ]
    write(OUT / "glyphs.h", lines)


def write_family(family: str, built: list) -> int:
    label = FAMILIES[family]["label"]
    lic = FAMILIES[family]["license"][1]
    what = f"{label}, SIL OFL 1.1, see {lic}"
    header = banner(what) + [
        "#pragma once",
        "#include <Arduino.h>  // gfxfont.h needs stdint + PROGMEM",
        "#include <gfxfont.h>",
        "",
        '#include "ui/fonts/glyphs.h"',
        "",
        "namespace ui::fonts {",
        "",
    ]
    source = banner(what) + [
        f'#include "ui/fonts/{family}.h"',
        "",
        "namespace ui::fonts {",
        "namespace {",
        "",
    ]
    total = 0
    for name, px, desc, (first, last, line, bitmap, glyphs) in built:
        ident = name[1:]
        source.append(f"// {desc} at {px} px, slots 0x{first:02X}-0x{last:02X}")
        source.append(f"const uint8_t {ident}Bitmaps[] PROGMEM = {{")
        for i in range(0, len(bitmap), 16):
            source.append("    " + ", ".join(f"0x{b:02X}" for b in bitmap[i : i + 16]) + ",")
        source.append("};")
        source.append(f"const GFXglyph {ident}Glyphs[] PROGMEM = {{")
        for off, w, h, adv, xo, yo, slot in glyphs:
            source.append(f"    {{{off}, {w}, {h}, {adv}, {xo}, {yo}}},  // 0x{slot:02X} {c_char_comment(slot)}")
        source += ["};", ""]
        header.append(f"extern const GFXfont {name};  // {desc}, {px} px, line {line} px")
        total += len(bitmap) + 7 * len(glyphs)
        print(f"{name:24s} {len(bitmap):6d} B bitmap, {len(glyphs):3d} glyphs, line {line}")
    source += ["}  // namespace", ""]
    for name, px, desc, (first, last, line, bitmap, glyphs) in built:
        ident = name[1:]
        source.append(
            f"const GFXfont {name} PROGMEM = {{const_cast<uint8_t *>({ident}Bitmaps), "
            f"const_cast<GFXglyph *>({ident}Glyphs), 0x{first:02X}, 0x{last:02X}, {line}}};"
        )
    source += ["", "}  // namespace ui::fonts"]
    header += ["", "}  // namespace ui::fonts"]
    write(OUT / f"{family}.h", header)
    write(OUT / f"{family}.cpp", source)
    return total


def main() -> None:
    src = Sources()
    OUT.mkdir(parents=True, exist_ok=True)
    write_glyphs_header()
    total = 0
    for family in FAMILIES:
        (OUT / FAMILIES[family]["license"][1]).write_bytes(src.license(family))
        built = []
        for name, fam, px, charset, opts in FONTS:
            if fam != family:
                continue
            desc = (f"weight {opts['weight']}" if fam == "jbmono"
                    else f"wght {opts['wght']}{' italic' if opts['italic'] else ''}")
            built.append((name, px, desc, build_font(name, src.face(fam, opts, px), charset)))
        total += write_family(family, built)
    print(f"total ~{total / 1024:.1f} KB flash -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
