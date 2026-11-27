# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy", "shapely>=2.0"]
# ///
"""The flatter back, from the v0.6 solids in ../model/ereader-enclosure.step (a mock-up: the ScanRuler
model is untouched). The back rises to the field level everywhere except a pod round the battery,
the pod is its own part (a battery tub) so every part prints flat with no supports, and a 0.8 mm
45° bevel runs round the back edge and the front bezel's front edge. The tray carries its revision
(REV), engraved on the field. Writes back-tray.stl,
battery-tub.stl and front-bezel.stl to enclosure/ (the print meshes, in place of v0.6's), the
joint's test coupons to ../coupons/, flatter-back.step to ../model/, and v0.6's meshes and the
battery and speaker boxes to ../build/ for pictures.py.

    uv run enclosure/tools/build.py

The tub drops in from the back and slides toward the USB end to lock. Its rim laps onto the field
outside, so a push on the back (the case lying on a table) goes into the tray, not into the battery
and board. Solid lugs on its long walls come up through notches in the tray's floor and, slid home,
sit over seats on the floor: nothing has to bend to hold the tub in. A finger cut from the floor's
edge springs into a notch in the tub's wall and stops it sliding back. The first two versions held
the tub with flexing latches (barbs, then tongues in windows): both let go.
"""
from pathlib import Path
import cadquery as cq, math, json

OUT = Path(__file__).resolve().parents[1]      # enclosure/: the print meshes go at its root
BUILD = OUT / "build"; BUILD.mkdir(exist_ok=True)
MODEL, COUPONS = OUT / "model", OUT / "coupons"
STEP = MODEL / "ereader-enclosure.step"

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
# on the floor reaches y 5.3) and the ESP32 module (its shield starts about y 22). Nothing hangs below
# the board to y 16.6, and small parts to z -2.4 from there: 3.6 of height over the floor under those,
# less 0.3 clear. Its thickness isn't given; 3.0 is drawn.
SPK = dict(x0=85.4, y0=7.8, l=14.0, w=10.0, t=3.0)
# speaker grille: slots at 45°, parallel to the frog's hatch (its edges run along +X+Y), clipped to a
# patch 2 in from the speaker's edges; a piece shorter than min_l at a corner is left out
GRILLE = dict(w=0.8, pitch=2.0, patch=(10.0, 6.0), min_l=2.0)

# The revision, engraved on the tray. v0.6 has it where the pod's corner is now, so here it stands on
# the field at the antenna end: centred between the pod's rim and the tray's edge, `over` above the
# speaker grille. The glyphs are v0.6's (revision_glyphs.py), read the same way as the frog
# (+X to -X, thumbwheel side up) and cut the same: `d` deep with 45° walls, in the full floor.
# `n` is the digit, v0.n. The ScanRuler model is v0.6; this back is the next print, so 7.
REV = dict(n=7, over=3.0, d=0.4)

# --- the split: the tray's opening and the tub in it ----------------------------------------------
# The tub drops in from the back and slides SLIDE toward the USB end to lock. Insets are measured in
# from the hatch outline (negative: outside it).
GROW = 1.2                     # the pod's edge at the field stands GROW outside the hatch outline
LAND = 0.4                     # the pod's edge is vertical for LAND below the field, then the 45° flank
OPEN_IN = 0.6                  # the tray's opening: the pod's rim laps GROW + OPEN_IN onto the field
EF = 0.3                       # a 45° chamfer on the opening's bed edge, clear of elephant's foot
GAP = 0.15                     # between the opening and the tub's wall through it
SLIDE = 5.0                    # the opening, and the pod that covers it, run on SLIDE toward the antenna end
WALL_IN = OPEN_IN + GAP        # the outer face of the tub's wall through the opening
POCKET_IN = 2.6                # the pocket's face
# The lugs: solid, on the tub's long walls, above the tray's floor. A lug's underside is 45° (it
# prints as an overhang) and sits `play` over a seat of the same slope on the tray: the seat starts
# on the opening's edge at z0 and rises `rise` outward, on a rail on the floor. The x given is the
# lug's USB-end end when locked; its notch in the tray is SLIDE further on. The board leaves 2.4 or
# more above the floor at each (depthmap.py), and 1.0 under the MX1.25 headers (PWR side, x 30 to 60).
LUG = dict(l=4.0, out=1.3, top=-4.6, play=0.1)
SEAT = dict(z0=-6.4, rise=1.2, flat=1.0, over=0.5, lead=(1.2, 0.3))   # lead: the last 1.2, to the notch, falls 0.3
LUGS = {"y0": [12.0, 24.0, 63.0], "y1": [12.0, 40.0, 63.0]}
NOTCH = 0.3                    # clearance round a lug in its notch
# The lock: a finger cut from the floor's edge on the thumbwheel side, fixed at its antenna end, with
# a tooth on its tip that springs into a notch in the tub's wall when the tub is home. The tub
# pushes on it endways (it's a strut), so it stops the tub sliding back without bending. Going in,
# the tub's wall pushes the tooth aside by its 45° underside. To let the tub out, push the tip away
# from the tub, from inside.
LOCK = dict(side="y1", tip=24.0, root=38.5, w=1.2, slit=0.4, tooth=1.0, tooth_l=2.0, top=-5.0, block=3.0, play=0.15)

# --- the bevels ------------------------------------------------------------------------------------
# 45°, S high, round the back edge and the front bezel's front edge. The lowest fin groove starts
# 0.9 above the field and the USB-C opening 1.2 above it, so 0.8 keeps both. The back bevel skips
# the PWR / BOOT flexures.
S = 0.8
Z_FRONT = 2.75                 # the front bezel's front face
XO0, XO1, YO0, YO1 = -2.4, 101.9, -2.25, 62.25       # tray walls
MX, MY = (XO0 + XO1) / 2, (YO0 + YO1) / 2            # the outline is symmetric about these
TABS = (69.35, 88.55)

def hatch(inset, run=0.0):
    """The hatch outline (a chamfered rectangle) offset inward by `inset`, mitred; `run` longer at
    the antenna end."""
    w, h, c = HW - 2 * inset, HH - 2 * inset, HCH - inset * math.tan(math.radians(22.5))
    x0, x1, y0, y1 = BCX - w / 2, BCX + w / 2 + run, BCY - h / 2, BCY + h / 2
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

# The pod back. The pod: GROW outside the hatch outline at the field (and SLIDE longer), a LAND of
# vertical edge, then 45° flanks running down to its face. The flanks' top outline is carried straight
# up for the edge (the taper rounds the corners a little, so the hatch outline itself would overhang).
pod = (prism(hatch(RISE - LAND - GROW, SLIDE), Z_BACK, Z_FIELD - LAND, taper=-45)
       .faces(">Z").wires().toPending().extrude(LAND))
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

# The split. The tray: everything from the field up, less the opening. The tub: the pod below the
# field, and the wall through the opening.
back = cutter(BACK, skip=[("y0", *TABS)])
opening = (prism(hatch(OPEN_IN, SLIDE), Z_FIELD - 1, 5)
           .union(prism(hatch(OPEN_IN - EF, SLIDE), Z_FIELD, Z_FIELD + EF, taper=45)))
tray = pod_tray.cut(below).cut(opening).cut(back)
bezel = pod_bezel.cut(back).cut(cutter(FRONT))
tub = pod_tray.intersect(below.union(prism(hatch(WALL_IN), Z_FIELD - 0.5, 5)))

def sweep(pts, vec):
    """A prism: the polygon `pts` (3D) swept along `vec`."""
    face = cq.Face.makeFromWires(cq.Wire.makePolygon([cq.Vector(*p) for p in pts], close=True))
    return cq.Workplane().add(cq.Solid.extrudeLinear(face, cq.Vector(*vec)))

# Along a long side: i the inset from the hatch outline, s along it (x)
EDGE = {"y0": BCY - HH / 2, "y1": BCY + HH / 2}
def Y(side, i): return EDGE[side] + i if side == "y0" else EDGE[side] - i
def ibox(side, i0, i1, s0, s1, z0, z1):
    ya, yb = sorted((Y(side, i0), Y(side, i1))); return box(s0, ya, z0, s1, yb, z1)
def along(side, prof, s0, s1):
    """An (inset, z) profile swept along the side."""
    return sweep([(s0, Y(side, i), z) for i, z in prof], (s1 - s0, 0, 0))
def loft(side, prof0, s0, prof1, s1):
    """From one (inset, z) profile at s0 to another at s1, ruled."""
    ws = [cq.Wire.makePolygon([cq.Vector(s, Y(side, i), z) for i, z in prof], close=True)
          for prof, s in ((prof0, s0), (prof1, s1))]
    return cq.Workplane().add(cq.Solid.makeLoft(ws, True))

z_top = SEAT["z0"] + SEAT["rise"]                                   # the rail's top
seat_z = lambda i: SEAT["z0"] + OPEN_IN - i                         # the seat's slope
i_tip = WALL_IN - LUG["out"]                                        # the lug's outer face
def over_seat(drop):
    """What's above the seat's slope (`drop` lower), as a profile."""
    i0 = OPEN_IN - SEAT["rise"] - drop
    return [(i0, z_top), (OPEN_IN + 0.5, seat_z(OPEN_IN + 0.5) - drop), (OPEN_IN + 0.5, 5), (i0, 5)]
features = []                                                       # (name, x0, y0, x1, y1, top) for the room check
for side, starts in LUGS.items():
    for h0 in starts:
        h1 = h0 + LUG["l"]
        # the tub's lug: the wall raised to the lug's top, and the lug out from it over the seat
        tub = tub.union(ibox(side, WALL_IN, POCKET_IN, h0, h1, Z_FTOP - 0.02, LUG["top"]))
        tub = tub.union(along(side, [(WALL_IN + 0.5, seat_z(WALL_IN + 0.5) + LUG["play"]), (i_tip, seat_z(i_tip) + LUG["play"]),
                                     (i_tip, LUG["top"]), (WALL_IN + 0.5, LUG["top"])], h0, h1))
        # the tray's seat: a rail on the floor beside the opening, its slope cut down to the opening's edge
        a, b = h0 - SEAT["over"], h0 + SLIDE - NOTCH              # as far as the notch, so the lug slides straight on
        ll, ld = SEAT["lead"]
        tray = tray.union(ibox(side, OPEN_IN - SEAT["rise"] - SEAT["flat"], OPEN_IN, a, b, Z_FTOP - 0.02, z_top))
        tray = tray.cut(along(side, over_seat(0.0), a - 0.01, b - ll)).cut(loft(side, over_seat(0.0), b - ll, over_seat(ld), b + 0.01))
        # the notch the lug comes up through, SLIDE on
        tray = tray.cut(ibox(side, i_tip - NOTCH, OPEN_IN + 0.01, h0 + SLIDE - NOTCH, h1 + SLIDE + NOTCH, Z_FIELD - 1, 5))
        ya, yb = sorted((Y(side, i_tip), Y(side, POCKET_IN)))
        features.append((f"lug {side} x {h0:g}", h0, ya, h1, yb, LUG["top"]))

# the lock
L = LOCK; sd = L["side"]
i_f = OPEN_IN - L["w"]                                              # the finger's back
tray = tray.union(ibox(sd, i_f, OPEN_IN, L["tip"], L["tip"] + L["block"], Z_FTOP - 0.02, L["top"]))
tray = tray.union(along(sd, [(OPEN_IN - 0.02, Z_FIELD), (OPEN_IN + L["tooth"], Z_FIELD + L["tooth"] + 0.02),
                             (OPEN_IN + L["tooth"], L["top"]), (OPEN_IN - 0.02, L["top"])], L["tip"], L["tip"] + L["tooth_l"]))
tray = (tray.cut(ibox(sd, i_f - L["slit"], i_f, L["tip"] - L["slit"], L["root"], Z_FIELD - 1, 5))
            .cut(ibox(sd, i_f - L["slit"], OPEN_IN + 0.01, L["tip"] - L["slit"], L["tip"], Z_FIELD - 1, 5)))
no_tooth = tray.cut(ibox(sd, OPEN_IN, OPEN_IN + L["tooth"] + 0.1, L["tip"] - 0.1, L["tip"] + L["tooth_l"] + 0.1, Z_FIELD - 1, 5))
tub = tub.union(ibox(sd, WALL_IN, POCKET_IN, L["tip"] - 2.5, L["tip"] + L["tooth_l"] + 2.5, Z_FTOP - 0.02, L["top"] + 0.2))
tub = tub.cut(ibox(sd, WALL_IN - 0.01, OPEN_IN + L["tooth"] + 0.25, L["tip"] - L["play"], L["tip"] + L["tooth_l"] + 0.3, Z_FIELD, 5))
ya, yb = sorted((Y(sd, i_f), Y(sd, POCKET_IN)))
features.append(("lock", L["tip"] - 2.5, ya, L["tip"] + L["tooth_l"] + 2.5, yb, L["top"] + 0.2))

# the revision
import revision_glyphs as rg                                         # beside this script
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

def groove(poly, z, d, g=0.05):
    """A groove cut in (+z) from a face at z: `poly` at the face, d deep, walls 45°. It starts g outside
    the face and g fatter, so the cut is clean there. Each corner moves along its mitre, which holds
    while no edge closes up on the way in (the revision's volume is checked below)."""
    def ring(r, grow, zz):
        pts, out = list(r.coords)[:-1], []
        for i, b in enumerate(pts):
            n = []                                                   # the two edges' normals, out of the material
            for (x0, y0), (x1, y1) in ((pts[i - 1], b), (b, pts[(i + 1) % len(pts)])):
                l = math.hypot(x1 - x0, y1 - y0); n.append(((y1 - y0) / l, (x0 - x1) / l))
            k = grow / (1 + n[0][0] * n[1][0] + n[0][1] * n[1][1])
            out.append(cq.Vector(b[0] + k * (n[0][0] + n[1][0]), b[1] + k * (n[0][1] + n[1][1]), zz))
        return out
    poly = orient(poly.simplify(1e-6), 1.0)                          # outline anticlockwise, counters clockwise
    rings = [poly.exterior, *poly.interiors]
    lo, hi = [ring(r, g, z - g) for r in rings], [ring(r, -d, z + d) for r in rings]
    faces = []
    for level in (lo, hi):
        wires = [cq.Wire.makePolygon(pts, close=True) for pts in level]
        faces.append(cq.Face.makeFromWires(wires[0], wires[1:]))
    for a, b in zip(lo, hi):
        for i in range(len(a)):
            j = (i + 1) % len(a)
            faces.append(cq.Face.makeFromWires(cq.Wire.makePolygon([a[i], a[j], b[j], b[i]], close=True)))
    return cq.Workplane().add(cq.Solid.makeSolid(cq.Shell.makeShell(faces)))

rev_text = f"v0.{REV['n']}"
placed = rg.layout(rev_text)
rev_left = (BCX + HW / 2 + GROW + SLIDE + XO1) / 2 + (placed[-1][1] + rg.W) / 2      # its reading-left edge, at +X
rev_base = SPK["y0"] + SPK["w"] / 2 + GRILLE["patch"][1] / 2 + REV["over"]
rev_plan = unary_union([rg.to_world(rg.GLYPHS[ch], gx, rev_left, rev_base) for ch, gx in placed])
tray_plain = tray
for glyph in rev_plan.geoms: tray = tray.cut(groove(glyph, Z_FIELD, REV["d"]))

def stl(shape, path):
    cq.exporters.export(shape, str(path), tolerance=0.02, angularTolerance=0.2)

parts = {"back-tray": tray, "battery-tub": tub, "front-bezel": bezel}
for name, shape in parts.items(): stl(shape, OUT / f"{name}.stl")
asm = cq.Assembly()
for name, shape in parts.items(): asm.add(shape, name=name)
cq.exporters.export(asm.toCompound(), str(MODEL / "flatter-back.step"))
stl(v06_tray, BUILD / "v06_tray.stl"); stl(v06_bezel, BUILD / "v06_bezel.stl")
spk = box(SPK["x0"], SPK["y0"], Z_FTOP, SPK["x0"] + SPK["l"], SPK["y0"] + SPK["w"], Z_FTOP + SPK["t"])
batt = box(BATT[0], BATT[1], Z_POCKET, BATT[2], BATT[3], Z_POCKET + BATT[4])
cq.exporters.export(spk, str(BUILD / "speaker.stl")); cq.exporters.export(batt, str(BUILD / "battery.stl"))

# Test coupons of the joint, to print before the whole parts: the USB-end 32 mm of the tub and the
# floor beside it, with the middle (COUPON's y band) taken out and the two sides closed up. Three
# lugs with their seats and notches, and the lock.
COUPON = dict(band=(13.0, 49.2), tub=(8.0, 40.0), tray=(7.85, 46.0), bar=4.0, y=(1.0, 60.0))
c = COUPON; shut = c["band"][1] - c["band"][0]
def squeeze(shape, x0, x1, ya, yb):
    near = shape.intersect(box(x0, ya, -20, x1, c["band"][0], 20))
    far = shape.intersect(box(x0, c["band"][1], -20, x1, yb, 20)).translate((0, -shut, 0))
    return near.union(far)
coupon_tub = squeeze(tub, *c["tub"], -5, 70)
coupon_tray = squeeze(tray, *c["tray"], *c["y"])
for xa, xb in ((c["tray"][0] - c["bar"], c["tray"][0]), (c["tray"][1], c["tray"][1] + c["bar"])):
    coupon_tray = coupon_tray.union(box(xa, c["y"][0], Z_FIELD, xb, c["y"][1] - shut, Z_FTOP))
stl(coupon_tub, COUPONS / "coupon-tub.stl"); stl(coupon_tray, COUPONS / "coupon-tray.stl")

def vol(w): return round(sum(v.Volume() for v in w.vals()), 4)
# the way in: up through the notches SLIDE toward the antenna end, then along to home. The tooth is
# left out (it springs aside); every other overlap should be nothing.
way_in = {f"up, {dz:g} to go": vol(tub.translate((SLIDE, 0, -dz)).intersect(no_tooth)) for dz in (3.0, 1.5, 0.5)}
way_in |= {f"slide, {d:g} to go": vol(tub.translate((d, 0, 0)).intersect(no_tooth)) for d in (5.0, 3.75, 2.5, 1.25)}
# room under the board over each raised feature (needs depthmap.py's map)
room = None
if (BUILD / "depth.npz").exists():
    import numpy as np
    d = np.load(BUILD / "depth.npz"); zmin = np.minimum(np.where(np.isfinite(d["zmin"]), d["zmin"], -1.6), -1.6)
    def under(x0, y0, x1, y1):
        m = (d["xs"][None, :] >= x0 - 0.25) & (d["xs"][None, :] <= x1 + 0.25) & (d["ys"][:, None] >= y0 - 0.25) & (d["ys"][:, None] <= y1 + 0.25)
        return float(zmin[m].min())
    room = {name: round(under(x0, y0, x1, y1) - top, 2) for name, x0, y0, x1, y1, top in features}
# the revision: what was cut against the glyphs' own volume (area is quadratic in depth), and that it
# stands on full floor, 1 clear all round
area = lambda h: rev_plan.buffer(-h, join_style="mitre").area
rb = rev_plan.bounds
revision = dict(
    text=rev_text, x=[round(rb[0], 2), round(rb[2], 2)], y=[round(rb[1], 2), round(rb[3], 2)],
    cut_mm3=round(tray_plain.val().Volume() - tray.val().Volume(), 3),
    glyphs_mm3=round(REV["d"] / 6 * (area(0) + 4 * area(REV["d"] / 2) + area(REV["d"])), 3),
    floor_left_mm=round(FLOOR - REV["d"], 2),
    not_floor_mm3=vol(box(rb[0] - 1, rb[1] - 1, Z_FIELD, rb[2] + 1, rb[3] + 1, Z_FTOP).cut(tray_plain)))
print(json.dumps(dict(
    mm3={k: round(v.val().Volume()) for k, v in parts.items()},
    v06_mm3=dict(tray=round(v06_tray.val().Volume()), bezel=round(v06_bezel.val().Volume())),
    valid={k: v.val().isValid() for k, v in parts.items()},
    solids={k: len(v.solids().vals()) for k, v in parts.items()},
    tray_tub_overlap_mm3=vol(tray.intersect(tub)), way_in_overlap_mm3=way_in, room_over_features_mm=room,
    coupons=dict(valid=[coupon_tub.val().isValid(), coupon_tray.val().isValid()],
                 solids=[len(coupon_tub.solids().vals()), len(coupon_tray.solids().vals())]),
    grille_mm2=round(grille_mm2, 1), revision=revision), indent=1))
