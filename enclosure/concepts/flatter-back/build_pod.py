# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy"]
# ///
"""Concept A, the pod back: the back rises to the field level everywhere except a pod round the
battery. Cut from the v0.6 solids in ../../ereader-enclosure.step (a mock-up: the ScanRuler model
is untouched). Writes podA_tray.stl, podA_bezel.stl and podA.step here, and the v0.6 meshes and the
battery and placeholder speaker boxes to build/ for render.py and sections.py.

    uv run enclosure/concepts/flatter-back/build_pod.py
"""
from pathlib import Path
import cadquery as cq, math, json

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"; BUILD.mkdir(exist_ok=True)
STEP = HERE.parents[1] / "ereader-enclosure.step"

Z_BACK = -9.11            # v0.6 flat back
Z_FIELD = -7.2            # standoff_z - back_t: set by the screws (and the USB-C opening + 1.2)
FLOOR = 1.2
Z_FTOP = Z_FIELD + FLOOR  # -6.0
BCX, BCY = 39.85, 33.8
HW, HH, HCH = 68.7, 48.2, 3.0          # hatch outline (hatch_w, hatch_h, hatch_ch)
RISE = Z_FIELD - Z_BACK                # 1.91
POCKET = (8.1, 12.3, 71.6, 55.3)       # battery pocket in plan
Z_POCKET = -8.31
HOLES = [(2.7, 2.7), (96.8, 2.7), (2.7, 57.3), (96.8, 57.3)]
# the speaker: 14 × 10, long side along the case, between the antenna end's corner screw (its boss
# on the floor reaches y 5.3) and the ESP32 module, where nothing hangs below the board: 4.1 of height
# over the floor, less 0.3 clear. Its thickness isn't given; 3.0 is drawn.
SPK = dict(x0=85.4, y0=5.8, l=14.0, w=10.0, t=3.0)
# speaker grille: slots at 45°, parallel to the frog's hatch (its edges run along +X+Y), clipped to a
# patch centred under the speaker; a piece shorter than min_l at a corner is left out
GRILLE = dict(w=0.8, pitch=2.0, patch=(10.0, 6.0), min_l=2.0)     # patch 2 in from the speaker's edges

def chamfer_rect(cx, cy, w, h, c):
    x0, x1, y0, y1 = cx - w/2, cx + w/2, cy - h/2, cy + h/2
    return [(x0+c, y0), (x1-c, y0), (x1, y0+c), (x1, y1-c), (x1-c, y1), (x0+c, y1), (x0, y1-c), (x0, y0+c)]

def prism(pts, z0, z1, taper=0.0):
    return cq.Workplane("XY", origin=(0, 0, z0)).polyline(pts).close().extrude(z1 - z0, taper=taper)

def box(x0, y0, z0, x1, y1, z1):
    return cq.Workplane("XY").box(x1-x0, y1-y0, z1-z0, centered=False).translate((x0, y0, z0))

def load_v06():
    """(tray, bezel) from the v0.6 STEP: the tray is the solid that reaches lowest."""
    s = sorted(cq.importers.importStep(str(STEP)).solids().vals(), key=lambda v: v.BoundingBox().zmin)
    return cq.Workplane().add(s[0]), cq.Workplane().add(s[1])

tray, bezel = load_v06()

# Pod: the hatch outline at the field level, its 45° flanks running down to a face RISE smaller a side.
face_ch = HCH - RISE * math.tan(math.radians(22.5))
pod = prism(chamfer_rect(BCX, BCY, HW - 2*RISE, HH - 2*RISE, face_ch), Z_BACK, Z_FIELD, taper=-45)
below = box(-10, -10, -20, 115, 75, Z_FIELD)
cutter = below.cut(pod)

pocket_all = box(POCKET[0], POCKET[1], Z_POCKET, POCKET[2], POCKET[3], 5)
hatch_field = prism(chamfer_rect(BCX, BCY, HW, HH, HCH), Z_FIELD, Z_FTOP)
cavity = (cq.Workplane("XY", origin=(0, 0, Z_FIELD)).center(49.75, 30.0)
          .rect(100.3, 60.5).extrude(FLOOR).edges("|Z").fillet(1.0))
# the PWR / BOOT flexures were rooted in the old floor: root them in the new one (tabs 1.9 shorter)
relief = box(70.35, -2.25, Z_FIELD, 87.65, -0.25, Z_FTOP)
# keep the frog: the pod's add mustn't refill the pocket or the engraving
pocket_full = box(POCKET[0], POCKET[1], -20, POCKET[2], POCKET[3], 5)
adds = pod.cut(pocket_full).union(hatch_field.union(cavity).union(relief).cut(pocket_all))

new_tray = tray.cut(cutter).union(adds)
# screws: Ø2.8 through, 90° countersink Ø6.0 at the new back (M2.5 × 4 screws, not × 6)
for (x, y) in HOLES:
    hole = cq.Workplane("XY", origin=(x, y, Z_FIELD - 1)).circle(1.4).extrude(5)
    csk = cq.Solid.makeCone(3.0 + 0.01, 1.4, 1.6, cq.Vector(x, y, Z_FIELD - 0.01), cq.Vector(0, 0, 1))
    new_tray = new_tray.cut(hole).cut(cq.Workplane().add(csk))
# keep the thumbwheel slot open to the back (its back wall is y 60, the cavity reached 60.25)
new_tray = new_tray.cut(box(73.63, 60.0, Z_FIELD - 1, 84.43, 61.0, Z_FTOP + 0.01))
# speaker grille under the placeholder (see GRILLE); the slots' 45° walls not modelled
gx, gy = SPK["x0"] + SPK["l"] / 2, SPK["y0"] + SPK["w"] / 2
PW, PH = GRILLE["patch"]
z0, z1 = Z_FIELD - 1, Z_FTOP + 0.01
patch = box(gx - PW / 2, gy - PH / 2, z0, gx + PW / 2, gy + PH / 2, z1)
grille_mm2 = 0.0
k_max = int((PW + PH) / 2 / math.sqrt(2) // GRILLE["pitch"])
for k in range(-k_max, k_max + 1):
    off = k * GRILLE["pitch"] / math.sqrt(2)             # along (-1, 1)/√2, square to the slots
    band = (cq.Workplane("XY", origin=(gx - off, gy + off, z0)).transformed(rotate=(0, 0, 45))
            .rect(40, GRILLE["w"]).extrude(z1 - z0))
    slot = band.intersect(patch)
    area = slot.val().Volume() / (z1 - z0) if slot.vals() else 0.0
    if area < GRILLE["w"] * GRILLE["min_l"]: continue
    new_tray = new_tray.cut(slot); grille_mm2 += area

new_bezel = bezel.cut(below)

def stl(shape, path):
    cq.exporters.export(shape, str(path), tolerance=0.02, angularTolerance=0.2)

stl(new_tray, HERE / "podA_tray.stl")
stl(new_bezel, HERE / "podA_bezel.stl")
cq.exporters.export(cq.Assembly().add(new_tray, name="tray").add(new_bezel, name="bezel").toCompound(), str(HERE / "podA.step"))
stl(tray, BUILD / "v06_tray.stl")
stl(bezel, BUILD / "v06_bezel.stl")
spk = box(SPK["x0"], SPK["y0"], Z_FTOP, SPK["x0"] + SPK["l"], SPK["y0"] + SPK["w"], Z_FTOP + SPK["t"])
batt = box(8.6, 12.8, Z_POCKET, 71.1, 54.8, Z_POCKET + 4.61)
cq.exporters.export(spk, str(BUILD / "speaker.stl")); cq.exporters.export(batt, str(BUILD / "battery.stl"))
vt, vb = new_tray.val().Volume(), new_bezel.val().Volume()
ot, ob = tray.val().Volume(), bezel.val().Volume()
print(json.dumps(dict(tray_mm3=[round(ot), round(vt)], bezel_mm3=[round(ob), round(vb)], grille_mm2=round(grille_mm2, 1),
                      valid=[new_tray.val().isValid(), new_bezel.val().isValid()],
                      solids=[len(new_tray.solids().vals()), len(new_bezel.solids().vals())])))
