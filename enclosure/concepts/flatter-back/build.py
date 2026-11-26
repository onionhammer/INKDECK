# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy"]
# ///
"""The flatter back, from the v0.6 solids in ../../ereader-enclosure.step (a mock-up: the ScanRuler
model is untouched). The back rises to the field level everywhere except a pod round the battery,
the pod is its own part (a battery tub) so every part prints flat with no supports, and a 0.8 mm
45° bevel runs round the back edge and the front bezel's front edge. Writes back-tray.stl,
battery-tub.stl and front-bezel.stl to enclosure/ (the print meshes, in place of v0.6's),
flatter-back.step here, and v0.6's meshes and the battery and speaker boxes to build/ for
pictures.py.

    uv run enclosure/concepts/flatter-back/build.py

The tub goes in from outside and sits on a 45° ledge round the opening, so a push on the back (the
case lying on a table) goes into the tray, not into the battery and board. Three latches hold it in.
Each is a beam cut out of the tub's long wall, fixed at both ends. A tongue on the tray reaches into
a window in it. Any catch printed on the tub's bed side is at most 45°, and a 45° barb across the
beam pushes the beam in when pulled (the first version let go at about 11 N). So the window's top is
a 45° arch running along the beam: pulled, it rests on the tongue's corners and pushes along the
beam, so the latch holds until the beam gives (about 68 N for the three, by rough beam estimates).
"""
from pathlib import Path
import cadquery as cq, math, json

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"; BUILD.mkdir(exist_ok=True)
STEP = HERE.parents[1] / "ereader-enclosure.step"
OUT = HERE.parents[1]      # the print meshes go at enclosure/

# --- the pod back -------------------------------------------------------------------------------
Z_BACK = -9.11            # v0.6's flat back, set by the battery
Z_FIELD = -7.2            # standoff_z - back_t: set by the screws (and the USB-C opening + 1.2)
FLOOR = 1.2
Z_FTOP = Z_FIELD + FLOOR  # -6.0
# The battery: about 65 × 45 with room for the wiring out of its side, about 4.5 thick. The tub
# gives it no more height than v0.5 does: 4.61 (batt_t) over the pocket floor, to z -3.7. The pocket is all the room the board leaves at that height: 65.0 long between
# the USB-C socket's pins (x 7.86, z -3.62) and the thumbwheel's body (x 72.8, for y over 50.4),
# 47.0 wide between the MX1.25 headers and the thumbwheel.
POCKET = (7.8, 7.6, 72.8, 54.6)
BATT = (7.8, 8.6, 72.8, 53.6, 4.5)     # the 65 × 45 envelope in it, centred across, and its thickness
Z_POCKET = -8.31
# the hatch outline (the seam, and the tub's outline) is the pocket and 2.6 round it, chamfer 3
BCX, BCY = (POCKET[0] + POCKET[2]) / 2, (POCKET[1] + POCKET[3]) / 2
HW, HH, HCH = POCKET[2] - POCKET[0] + 5.2, POCKET[3] - POCKET[1] + 5.2, 3.0
RISE = Z_FIELD - Z_BACK                                 # 1.91
HOLES = [(2.7, 2.7), (96.8, 2.7), (2.7, 57.3), (96.8, 57.3)]
# the speaker: 14 × 10, long side along the case, between the antenna end's corner screw (its boss
# on the floor reaches y 5.3) and the ESP32 module, where nothing hangs below the board: 4.1 of height
# over the floor, less 0.3 clear. Its thickness isn't given; 3.0 is drawn.
SPK = dict(x0=85.4, y0=5.8, l=14.0, w=10.0, t=3.0)
# speaker grille: slots at 45°, parallel to the frog's hatch (its edges run along +X+Y), clipped to a
# patch 2 in from the speaker's edges; a piece shorter than min_l at a corner is left out
GRILLE = dict(w=0.8, pitch=2.0, patch=(10.0, 6.0), min_l=2.0)

# --- the split: the tray's opening and the tub in it ----------------------------------------------
# seat: the tray's opening is the hatch outline up to z -6.6, then a 45° ledge narrows it by 0.6
Z_LEDGE, LEDGE = -6.6, 0.6
GAP = 0.15                     # seam gap round the tub; the 45° faces touch (that's the seat)
WALL_IN = LEDGE + 0.1          # the tub's outer face above the floor (0.1 clear of the ledge's edge)
# --- the latches ---------------------------------------------------------------------------------
# Three: one on the PWR side (between the corner and the MX1.25 headers, under the SD socket's end)
# and two on the thumbwheel side. Each is a beam cut out of the tub's long wall, fixed at both ends,
# that flexes in toward the pocket. A tongue on the tray reaches through the tub's skin into a window
# in the beam. A pull rests the window's top, a 45° arch along the beam, on the tongue's top corners:
# that pushes along the beam, not into it, so the latch doesn't let go. The tub comes out only from
# inside, by pressing the beams in. (side, the beam's free span x0 to x1, the window's x)
LATCHES = [(-1, 10.6, 28.6, 19.6), (1, 10.0, 28.0, 19.0), (1, 44.0, 64.0, 54.0)]
# Insets from the seam outline: the tub's skin (it keeps the seat) from its seam face to 0.95, a 0.4
# gap, the beam 1.0 thick, and the pocket widened to the beam's inner face along the span
SKIN_IN, BEAM_IN = 0.95, (1.35, 2.35)
END = 2.0                      # the beam's part above the floor top runs on 2 mm past the span, on posts
# The slit under the beam, inside the wall (hidden): sliced at 0.2, two empty layers, so the beam's
# first layer bridges the span 0.4 over the wall. With one it would sag onto it and fuse; with three,
# the profile, which supports bridges, puts support in the slit.
Z_SLIT = (-7.45, -7.02)
Z_TOP, Z_HUMP, HUMP = -4.2, -2.8, 1.7      # the beam's top, and a hump over the window (flat ±1.7, 45° sides)
# The tray's tongue: 1.6 wide, its underside 45° from the opening wall just above the field out to its
# tip 0.4 inside the beam's outer face; its top flat. The window: 2.2 wide, 0.1 behind the tip, 0.15
# of play round the tongue. The hump's outer top edge leads the tongue in at 30° off vertical.
TONGUE = dict(w=1.6, tip=1.75, z0=-7.15, top=-4.9, back=-1.0)
WIN = dict(w=2.2, depth=0.5, play=0.15)
LEAD = 30.0

# --- the bevels ------------------------------------------------------------------------------------
# 45°, S high, round the back edge and the front bezel's front edge. The lowest fin groove starts
# 0.9 above the field and the USB-C opening 1.2 above it, so 0.8 keeps both. The back bevel skips
# the PWR / BOOT flexures.
S = 0.8
Z_FRONT = 2.75                 # the front bezel's front face
XO0, XO1, YO0, YO1 = -2.4, 101.9, -2.25, 62.25       # tray walls
MX, MY = (XO0 + XO1) / 2, (YO0 + YO1) / 2            # the outline is symmetric about these
TABS = (69.35, 88.55)

def hatch(inset):
    """The hatch outline (a chamfered rectangle) offset inward by `inset`, mitred."""
    w, h, c = HW - 2 * inset, HH - 2 * inset, HCH - inset * math.tan(math.radians(22.5))
    x0, x1, y0, y1 = BCX - w / 2, BCX + w / 2, BCY - h / 2, BCY + h / 2
    return [(x0+c, y0), (x1-c, y0), (x1, y0+c), (x1, y1-c), (x1-c, y1), (x0+c, y1), (x0, y1-c), (x0, y0+c)]

def prism(pts, z0, z1, taper=0.0):
    return cq.Workplane("XY", origin=(0, 0, z0)).polyline(pts).close().extrude(z1 - z0, taper=taper)

def box(x0, y0, z0, x1, y1, z1):
    return cq.Workplane("XY").box(x1-x0, y1-y0, z1-z0, centered=False).translate((x0, y0, z0))

# Edge cutters. A profile is the region to cut, in (u, z): u the depth in from the outer face.
BACK = [(-1, Z_FIELD + S + 1), (S + 1, Z_FIELD - 1), (-1, Z_FIELD - 1)]
FRONT = [(-1, Z_FRONT - S - 1), (S + 1, Z_FRONT + 1), (-1, Z_FRONT + 1)]

def wedge(p0, p1, out, prof, e0=0.0, e1=0.0):
    """The profile swept along the outer face from p0 to p1 (plan), `out` its outward normal;
    e0 / e1 run it on past the ends (only past a convex corner)."""
    t = cq.Vector(p1[0] - p0[0], p1[1] - p0[1], 0); L = t.Length; t = t.normalized()
    n = cq.Vector(out[0], out[1], 0).normalized()
    o = cq.Vector(p0[0], p0[1], 0) - t * e0
    pts = [o - n * u + cq.Vector(0, 0, z) for u, z in prof]
    face = cq.Face.makeFromWires(cq.Wire.makePolygon(pts, close=True))
    return cq.Workplane().add(cq.Solid.extrudeLinear(face, t * (L + e0 + e1)))

def cutter(prof, skip=()):
    """The profile round the whole outline: the walls between the corner caps (less the `skip`
    spans on the long sides, as (side, x0, x1)) and each cap's three outer faces (0.8 proud)."""
    def spans(side, a, b):
        out, at = [], a
        for x0, x1 in sorted((x0, x1) for sd, x0, x1 in skip if sd == side): out.append((at, x0)); at = x1
        return out + [(at, b)]
    ws = [wedge((a, YO0), (b, YO0), (0, -1), prof) for a, b in spans("y0", 5.25, 94.25)]
    ws += [wedge((a, YO1), (b, YO1), (0, 1), prof) for a, b in spans("y1", 5.25, 94.25)]
    ws += [wedge((XO0, 5.4), (XO0, 54.6), (-1, 0), prof), wedge((XO1, 5.4), (XO1, 54.6), (1, 0), prof)]
    for sx in (1, -1):
        for sy in (1, -1):
            f = lambda x, y: (MX + sx * (x - MX), MY + sy * (y - MY))
            ws += [wedge(f(-3.2, 5.4), f(-3.2, -0.05), (-sx, 0), prof, 0, 2.0),
                   wedge(f(5.25, -3.05), f(-0.2, -3.05), (0, -sy), prof, 0, 2.0),
                   wedge(f(-3.2, -0.05), f(-0.2, -3.05), (-sx, -sy), prof, 2.0, 2.0)]
    cut = ws[0]
    for w in ws[1:]: cut = cut.union(w)
    return cut

# v0.6: the tray is the solid that reaches lowest
s = sorted(cq.importers.importStep(str(STEP)).solids().vals(), key=lambda v: v.BoundingBox().zmin)
v06_tray, v06_bezel = cq.Workplane().add(s[0]), cq.Workplane().add(s[1])

# The pod back. The pod: the hatch outline at the field level, its 45° flanks running down to a
# face RISE smaller a side.
pod = prism(hatch(RISE), Z_BACK, Z_FIELD, taper=-45)
below = box(-10, -10, -20, 115, 75, Z_FIELD)
pocket_all = box(POCKET[0], POCKET[1], Z_POCKET, POCKET[2], POCKET[3], 5)
hatch_field = prism(hatch(0), Z_FIELD, Z_FTOP)
cavity = (cq.Workplane("XY", origin=(0, 0, Z_FIELD)).center(49.75, 30.0)
          .rect(100.3, 60.5).extrude(FLOOR).edges("|Z").fillet(1.0))
# the PWR / BOOT flexures were rooted in the old floor: root them in the new one (tabs 1.9 shorter)
relief = box(70.35, -2.25, Z_FIELD, 87.65, -0.25, Z_FTOP)
# keep the frog: the pod's add mustn't refill the pocket or the engraving
pocket_full = box(POCKET[0], POCKET[1], -20, POCKET[2], POCKET[3], 5)
adds = pod.cut(pocket_full).union(hatch_field.union(cavity).union(relief).cut(pocket_all))
pod_tray = v06_tray.cut(below.cut(pod)).union(adds)
# v0.6's pocket is smaller: clear the new one down to its floor, and fill whatever v0.6 engraved on
# its back inside the tub's face (the old hatch line) but the frog
pod_tray = pod_tray.cut(pocket_all)
face = pod.intersect(box(-10, -10, Z_BACK - 1, 115, 75, Z_POCKET)).intersect(pocket_full)
frog = max(face.cut(v06_tray).solids().vals(), key=lambda v: v.Volume())
pod_tray = pod_tray.union(face.cut(cq.Workplane().add(frog)))
# screws: Ø2.8 through, 90° countersink Ø6.0 at the new back (M2.5 × 4 screws, not × 6)
for (x, y) in HOLES:
    hole = cq.Workplane("XY", origin=(x, y, Z_FIELD - 1)).circle(1.4).extrude(5)
    csk = cq.Solid.makeCone(3.0 + 0.01, 1.4, 1.6, cq.Vector(x, y, Z_FIELD - 0.01), cq.Vector(0, 0, 1))
    pod_tray = pod_tray.cut(hole).cut(cq.Workplane().add(csk))
# keep the thumbwheel slot open to the back (its back wall is y 60, the cavity reached 60.25)
pod_tray = pod_tray.cut(box(73.63, 60.0, Z_FIELD - 1, 84.43, 61.0, Z_FTOP + 0.01))
# speaker grille (see GRILLE); the slots' 45° walls not modelled
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
    pod_tray = pod_tray.cut(slot); grille_mm2 += area
pod_bezel = v06_bezel.cut(below)

# The split: the tray's opening, and the envelope of the tub inside it
opening = (prism(hatch(0), -20, Z_LEDGE).union(prism(hatch(0), Z_LEDGE, Z_FTOP, taper=45))
           .union(prism(hatch(LEDGE), Z_FTOP, 5)))
z_seat = Z_FTOP - (LEDGE - GAP)                       # where the tub's land meets the 45° seat
envelope = (prism(hatch(GAP), -20, z_seat).union(prism(hatch(GAP), z_seat, Z_FTOP, taper=45))
            .union(prism(hatch(WALL_IN), Z_FTOP, 5)))
back = cutter(BACK, skip=[("y0", *TABS)])
tray = pod_tray.cut(opening).cut(back)
bezel = pod_bezel.cut(back).cut(cutter(FRONT))
tub = pod_tray.intersect(envelope)

def sweep(pts, vec):
    """A prism: the polygon `pts` (3D) swept along `vec`."""
    face = cq.Face.makeFromWires(cq.Wire.makePolygon([cq.Vector(*p) for p in pts], close=True))
    return cq.Workplane().add(cq.Solid.extrudeLinear(face, cq.Vector(*vec)))

# latches. Insets are measured in from the seam outline: y0 side at BCY - HH/2, y1 side mirrored.
z_apex = TONGUE["top"] + WIN["play"] + TONGUE["w"] / 2        # the arch clears the tongue's corners by the play
z_spring = z_apex - WIN["w"] / 2
flex = TONGUE["tip"] + 0.05 - BEAM_IN[0]                      # how far the beam flexes in going in
k_lead = 1 / math.tan(math.radians(LEAD))
z_lead = Z_HUMP - flex * k_lead                               # the lead-in's foot on the beam's outer face
z_under = lambda i: TONGUE["z0"] + i                          # the tongue's underside at inset i
for side, x0, x1, xw in LATCHES:
    edge = BCY + side * HH / 2
    Y = lambda i: edge - side * i
    def ibox(a, b, xa, xb, za, zb):
        ya, yb = sorted((Y(a), Y(b))); return box(xa, ya, za, xb, yb, zb)
    along = lambda prof, xa, xb: sweep([(xa, Y(i), z) for i, z in prof], (xb - xa, 0, 0))       # (inset, z)
    across = lambda prof, a, b: sweep([(x, Y(a), z) for x, z in prof], (0, Y(b) - Y(a), 0))   # (x, z)
    # free the beam: the gap outside it, the slit under it, the pocket widened to its inner face
    tub = (tub.cut(ibox(SKIN_IN, BEAM_IN[0], x0, x1, Z_SLIT[0], 5))
              .cut(ibox(SKIN_IN, BEAM_IN[1], x0, x1, Z_SLIT[0], Z_SLIT[1]))
              .cut(ibox(BEAM_IN[1], 2.61, x0, x1, Z_POCKET - 0.01, 5)))
    # its part above the floor top, on posts the wall's width at both ends, and the hump over the window
    tub = tub.union(ibox(BEAM_IN[0], BEAM_IN[1], x0 - END, x1 + END, Z_FTOP - 0.02, Z_TOP))
    for xa, xb in ((x0 - END, x0), (x1, x1 + END)):
        tub = tub.union(ibox(WALL_IN, 2.6, xa, xb, Z_FTOP - 0.02, Z_TOP))
    hs = Z_HUMP - Z_TOP
    tub = tub.union(across([(xw - HUMP - hs, Z_TOP - 0.01), (xw + HUMP + hs, Z_TOP - 0.01),
                            (xw + HUMP, Z_HUMP), (xw - HUMP, Z_HUMP)], BEAM_IN[0], BEAM_IN[1]))
    # the lead-in on the hump's outer top edge
    tub = tub.cut(along([(BEAM_IN[0] - 1, z_lead - k_lead), (BEAM_IN[0] + 1.5, z_lead + 1.5 * k_lead),
                         (BEAM_IN[0] + 1.5, 0), (BEAM_IN[0] - 1, 0)], xw - HUMP - hs, xw + HUMP + hs))
    # the window: up to the arch along the beam, its floor 45° across it, the play under the tongue;
    # and the notch in the skin the tongue passes through, the same floor
    above = along([(-1, z_under(-1) - WIN["play"]), (3, z_under(3) - WIN["play"]), (3, 5), (-1, 5)], xw - 3, xw + 3)
    hw = WIN["w"] / 2
    window = across([(xw - hw, -9), (xw + hw, -9), (xw + hw, z_spring), (xw, z_apex), (xw - hw, z_spring)],
                    BEAM_IN[0] - 0.01, BEAM_IN[0] + WIN["depth"])
    notch = ibox(0, SKIN_IN + 0.01, xw - TONGUE["w"] / 2 - 0.2, xw + TONGUE["w"] / 2 + 0.2, -10, 5)
    tub = tub.cut(window.intersect(above)).cut(notch.intersect(above))
    # the tray's tongue: from the opening wall and the floor top, out to its tip
    t = TONGUE
    tray = tray.union(along([(t["back"], Z_FTOP - 0.02), (-0.02, Z_FTOP - 0.02), (-0.02, z_under(-0.02)),
                             (t["tip"], z_under(t["tip"])), (t["tip"], t["top"]), (t["back"], t["top"])],
                            xw - t["w"] / 2, xw + t["w"] / 2))

def stl(shape, path):
    cq.exporters.export(shape, str(path), tolerance=0.02, angularTolerance=0.2)

parts = {"back-tray": tray, "battery-tub": tub, "front-bezel": bezel}
for name, shape in parts.items(): stl(shape, OUT / f"{name}.stl")
asm = cq.Assembly()
for name, shape in parts.items(): asm.add(shape, name=name)
cq.exporters.export(asm.toCompound(), str(HERE / "flatter-back.step"))
stl(v06_tray, BUILD / "v06_tray.stl"); stl(v06_bezel, BUILD / "v06_bezel.stl")
spk = box(SPK["x0"], SPK["y0"], Z_FTOP, SPK["x0"] + SPK["l"], SPK["y0"] + SPK["w"], Z_FTOP + SPK["t"])
batt = box(BATT[0], BATT[1], Z_POCKET, BATT[2], BATT[3], Z_POCKET + BATT[4])
cq.exporters.export(spk, str(BUILD / "speaker.stl")); cq.exporters.export(batt, str(BUILD / "battery.stl"))
clash = tray.intersect(tub).vals()
print(json.dumps(dict(
    mm3={k: round(v.val().Volume()) for k, v in parts.items()},
    v06_mm3=dict(tray=round(v06_tray.val().Volume()), bezel=round(v06_bezel.val().Volume())),
    valid={k: v.val().isValid() for k, v in parts.items()},
    solids={k: len(v.solids().vals()) for k, v in parts.items()},
    tray_tub_overlap_mm3=round(clash[0].Volume(), 4) if clash else 0.0, grille_mm2=round(grille_mm2, 1))))
