# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy"]
# ///
"""Concept A2: the pod back of A, split on the hatch outline into a tray and a battery tub so every
part prints flat with no supports, with a 0.8 mm 45° bevel round the back edge and the same on the
top cover's front edge (0.5 in A), so it sits softer in the hand. Cut from podA.step (run
build_pod.py first): a mock-up, the ScanRuler model is untouched. Writes a2_tray.stl, a2_tub.stl,
a2_bezel.stl and a2.step here.

    uv run enclosure/concepts/flatter-back/build_a2.py

The tub goes in from outside and sits on a 45° ledge round the opening, so a push on the back (the
case lying on a table) goes into the tray, not into the battery and board. Two snap latches on its
long sides hold it in: a beam fixed at both ends, freed from the tub by a slit under it (a bridge
when printed), with a barb that springs out over the tray's floor. The barb is 45° on both faces:
a flat catch would be a ledge in mid-air, and the slicer supports it. So it's a soft latch, one a
firm pull undoes (about 10 N for the pair, by a rough beam estimate).
"""
from pathlib import Path
import math, json
import cadquery as cq

HERE = Path(__file__).resolve().parent
STEP = HERE / "podA.step"

Z_BACK, Z_FIELD, Z_FTOP = -9.11, -7.2, -6.0          # pod face, field (A's back), floor top
BCX, BCY, HW, HH, HCH = 39.85, 33.8, 68.7, 48.2, 3.0  # hatch outline, the seam
# seat: the tray's opening is the hatch outline up to z -6.6, then a 45° ledge narrows it by 0.6
Z_LEDGE, LEDGE = -6.6, 0.6
GAP = 0.15                     # seam gap round the tub; the 45° faces touch (that's the seat)
WALL_IN = LEDGE + 0.1          # the tub's outer face above the floor (0.1 clear of the ledge's edge)
# latches: one on each long side, centred on the hatch
L_X0, L_X1 = BCX - 12.0, BCX + 12.0   # beam, 2 mm of it fixed at each end
SLIT_X0, SLIT_X1 = BCX - 10.0, BCX + 10.0
# The slit under the beam runs from z -6.55 up to the beam at -6.02: sliced at 0.2, two empty
# layers, so the beam's first layer (a 20 mm bridge) goes down 0.4 over the rim. With one empty layer
# (0.2) it would sag onto the rim and fuse the latch; with three, the profile, which supports
# bridges, puts support in the slit.
BEAM_T, Z_BEAM_TOP, Z_SLIT = 1.0, -4.6, -6.55
BARB_W, BARB_OUT = 8.0, 0.6     # the barb overlaps the floor by 0.5, with 0.1 of play over its edge
# A2's bevels: 45°, S high, round the back edge and the top cover's front edge. The lowest fin
# groove starts 0.9 above the field and the USB-C opening 1.2 above it, so 0.8 keeps both. The
# back bevel skips the PWR / BOOT flexures, as B does.
S = 0.8
Z_FRONT = 2.75                 # the top cover's front face
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
        cuts = sorted((x0, x1) for sd, x0, x1 in skip if sd == side)
        out, at = [], a
        for x0, x1 in cuts: out.append((at, x0)); at = x1
        return out + [(at, b)]
    ws = [wedge((a, YO0), (b, YO0), (0, -1), prof) for a, b in spans("y0", 5.25, 94.25)]
    ws += [wedge((a, YO1), (b, YO1), (0, 1), prof) for a, b in spans("y1", 5.25, 94.25)]
    ws += [wedge((XO0, 5.4), (XO0, 54.6), (-1, 0), prof), wedge((XO1, 5.4), (XO1, 54.6), (1, 0), prof)]
    E = 2.0
    for sx in (1, -1):
        for sy in (1, -1):
            f = lambda x, y: (MX + sx * (x - MX), MY + sy * (y - MY))
            ws += [wedge(f(-3.2, 5.4), f(-3.2, -0.05), (-sx, 0), prof, 0, E),
                   wedge(f(5.25, -3.05), f(-0.2, -3.05), (0, -sy), prof, 0, E),
                   wedge(f(-3.2, -0.05), f(-0.2, -3.05), (-sx, -sy), prof, E, E)]
    cut = ws[0]
    for w in ws[1:]: cut = cut.union(w)
    return cut

s = sorted(cq.importers.importStep(str(STEP)).solids().vals(), key=lambda v: v.BoundingBox().zmin)
podA_tray, podA_bezel = cq.Workplane().add(s[0]), cq.Workplane().add(s[1])

# the tray's opening, and the envelope of the tub inside it
opening = (prism(hatch(0), -20, Z_LEDGE).union(prism(hatch(0), Z_LEDGE, Z_FTOP, taper=45))
           .union(prism(hatch(LEDGE), Z_FTOP, 5)))
z_seat = Z_FTOP - (LEDGE - GAP)                       # where the tub's land meets the 45° seat
envelope = (prism(hatch(GAP), -20, z_seat).union(prism(hatch(GAP), z_seat, Z_FTOP, taper=45))
            .union(prism(hatch(WALL_IN), Z_FTOP, 5)))
leftover = podA_tray.intersect(prism(hatch(0), Z_FTOP, 5)).val().Volume()

tray0 = podA_tray.cut(opening)
back = cutter(BACK, skip=[("y0", *TABS)])
tray, bezel = tray0.cut(back), podA_bezel.cut(back).cut(cutter(FRONT))
tub = podA_tray.intersect(envelope)

# latches. y0 side: hatch edge at BCY - HH/2, outward -y; y1 side mirrored.
for side in (-1, 1):
    edge = BCY + side * HH / 2
    yo, yi = edge - side * WALL_IN, edge - side * (WALL_IN + BEAM_T)     # beam's outer / inner face
    beam = box(L_X0, min(yo, yi), Z_FTOP - 0.02, L_X1, max(yo, yi), Z_BEAM_TOP)
    ring_o, ring_i = edge - side * GAP, edge - side * 2.6                # the tub's rim, to the pocket
    slit = box(SLIT_X0, min(ring_o, ring_i) - 0.01, Z_SLIT, SLIT_X1, max(ring_o, ring_i) + 0.01, Z_FTOP)
    tip = edge - side * (WALL_IN - BARB_OUT)
    yb = yo - side * 0.01                                              # into the beam
    barb = (cq.Workplane("YZ", origin=(BCX - BARB_W / 2, 0, 0))
            .polyline([(yb, Z_FTOP), (tip, Z_FTOP + BARB_OUT), (tip, Z_FTOP + BARB_OUT + 0.1),
                       (yb, Z_FTOP + 2 * BARB_OUT + 0.1)])
            .close().extrude(BARB_W))
    tub = tub.cut(slit).union(beam).union(barb)

def stl(shape, path):
    cq.exporters.export(shape, str(path), tolerance=0.02, angularTolerance=0.2)

stl(tray, HERE / "a2_tray.stl"); stl(tub, HERE / "a2_tub.stl"); stl(bezel, HERE / "a2_bezel.stl")
cq.exporters.export(cq.Assembly().add(tray, name="tray").add(tub, name="tub").add(bezel, name="bezel")
                    .toCompound(), str(HERE / "a2.step"))
clash = tray.intersect(tub).vals()
parts = dict(tray=tray, tub=tub, bezel=bezel)
print(json.dumps(dict(
    mm3={k: round(v.val().Volume()) for k, v in parts.items()},
    was=dict(tray=round(podA_tray.val().Volume()), bezel=round(podA_bezel.val().Volume())),
    valid={k: v.val().isValid() for k, v in parts.items()},
    solids={k: len(v.solids().vals()) for k, v in parts.items()},
    tray_tub_overlap_mm3=round(clash[0].Volume(), 4) if clash else 0.0,
    podA_inside_opening_above_floor_mm3=round(leftover, 3))))
