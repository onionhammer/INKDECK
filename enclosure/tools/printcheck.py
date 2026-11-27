# /// script
# requires-python = ">=3.10"
# ///
"""Does each part need supports? Slices the flatter back's parts, its test coupons and v0.6's parts, as they'd lie on the
bed, with PrusaSlicer's own CORE One presets (the enclosure's printer, profile and PETG) and
automatic supports on at the profile's threshold, and counts the support extrusions in the G-code.
Run build.py first; gcode goes to build/.

    uv run enclosure/tools/printcheck.py
"""
from pathlib import Path
import re, subprocess, sys

OUT = Path(__file__).resolve().parents[1]   # enclosure/: the print meshes are at its root
BUILD = OUT / "build"
SLICER = "/Applications/Original Prusa Drivers/PrusaSlicer.app/Contents/MacOS/PrusaSlicer"
PRESETS = ["--printer-profile", "Prusa CORE One HF0.4 nozzle", "--print-profile", "0.20mm STRUCTURAL @COREONE 0.4",
           "--material-profile", "Generic PETG @COREONE HF0.4"]
# (name, mesh, extra args): trays and the tub print back-down as modelled, bezels face-down
PARTS = [("v0.6 tray", BUILD / "v06_tray.stl", []), ("v0.6 bezel", BUILD / "v06_bezel.stl", ["--rotate-x", "180"]),
         ("back tray", OUT / "back-tray.stl", []), ("battery tub", OUT / "battery-tub.stl", []),
         ("front bezel", OUT / "front-bezel.stl", ["--rotate-x", "180"]),
         ("coupon tray", OUT / "coupons/coupon-tray.stl", []), ("coupon tub", OUT / "coupons/coupon-tub.stl", [])]

def check(name, mesh, extra):
    out = BUILD / (re.sub(r"\W+", "_", name).strip("_") + ".gcode")
    r = subprocess.run([SLICER, "--export-gcode", "--binary-gcode=0", *PRESETS, "--support-material",
                        "--support-material-auto", *extra, "--output", str(out), str(mesh)],
                       capture_output=True, text=True)
    if r.returncode: sys.exit(f"{name}: PrusaSlicer failed\n{r.stdout[-800:]}{r.stderr[-800:]}")
    kind, z, moves, layers, cfg = None, 0.0, {}, set(), {}
    for line in out.open():
        if line.startswith(";Z:"): z = float(line[3:])
        elif line.startswith(";TYPE:"): kind = line[6:].strip()
        elif line.startswith("G1") and " E" in line and kind:
            moves[kind] = moves.get(kind, 0) + 1
            if kind.startswith("Support"): layers.add(z)
        elif m := re.match(r"; (support_material_threshold|dont_support_bridges) = (.*)", line):
            cfg[m[1]] = m[2].strip()
    sup = sum(v for k, v in moves.items() if k.startswith("Support"))
    print(f"{name:11s} support moves {sup:5d} on {len(layers):2d} layers   "
          f"(bridges {moves.get('Bridge infill', 0)}, overhang perimeters {moves.get('Overhang perimeter', 0)})")
    return cfg

cfg = {}
for p in PARTS: cfg = check(*p)
print(f"threshold {cfg.get('support_material_threshold')}°, bridges supported: {cfg.get('dont_support_bridges') == '0'}")
