# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy"]
# ///
"""Concept B, the bevelled edge: one-piece tray, flat back kept, a 45° bevel S high round the back
edge between the corner caps (skipping the PWR / BOOT flexures). Prints back-down with no supports.
Cut from the v0.5 solids in ../../ereader-enclosure.step (a mock-up). Writes bevelB_tray.stl here.

    uv run enclosure/concepts/flatter-back/build_bevel.py
"""
from pathlib import Path
import cadquery as cq

HERE = Path(__file__).resolve().parent
STEP = HERE.parents[1] / "ereader-enclosure.step"

S = 2.0
ZB = -9.11
XO0, XO1, YO0, YO1 = -2.4, 101.9, -2.25, 62.25
CAP = 7.5 + 0.15                       # bump_l + cap_gap: the bevel stops at the corner caps

def box(x0, y0, z0, x1, y1, z1):
    return cq.Workplane("XY").box(x1-x0, y1-y0, z1-z0, centered=False).translate((x0, y0, z0))

def wedge_x(x0, x1, yface, outward):   # along a long side; outward = -1 (y0 side) or +1
    d = -outward
    pts = [(yface + outward*1.0, ZB + S + 1.0), (yface + d*(S + 1.0), ZB - 1.0), (yface + outward*1.0, ZB - 1.0)]
    return cq.Workplane("YZ", origin=(x0, 0, 0)).polyline(pts).close().extrude(x1 - x0)

def wedge_y(y0, y1, xface, outward):   # along an end
    d = -outward
    pts = [(xface + outward*1.0, ZB + S + 1.0), (xface + d*(S + 1.0), ZB - 1.0), (xface + outward*1.0, ZB - 1.0)]
    return cq.Workplane("XZ", origin=(0, y1, 0)).polyline(pts).close().extrude(y1 - y0)

s = sorted(cq.importers.importStep(str(STEP)).solids().vals(), key=lambda v: v.BoundingBox().zmin)
tray = cq.Workplane().add(s[0])
# fill the lowest fin groove first (the bevel would leave a sliver of it); fins re-centre in a real build
fill = (box(XO0 + CAP, YO0, -8.1, 69.35, YO0 + 0.6, -7.2)
        .union(box(88.55, YO0, -8.1, XO1 - CAP, YO0 + 0.6, -7.2))
        .union(box(XO0 + CAP, YO1 - 0.6, -8.1, 71.6, YO1, -7.2))
        .union(box(86.4, YO1 - 0.6, -8.1, XO1 - CAP, YO1, -7.2))
        .union(box(XO1 - 0.6, YO0 + CAP, -8.1, XO1, YO1 - CAP, -7.2))
        .union(box(XO0, YO0 + CAP, -8.1, XO0 + 0.6, YO1 - CAP, -7.2)))
t = tray.union(fill)
cut = (wedge_x(XO0 + CAP, 69.35, YO0, -1).union(wedge_x(88.55, XO1 - CAP, YO0, -1))
       .union(wedge_x(XO0 + CAP, XO1 - CAP, YO1, +1))
       .union(wedge_y(YO0 + CAP, YO1 - CAP, XO1, +1)).union(wedge_y(YO0 + CAP, YO1 - CAP, XO0, -1)))
t = t.cut(cut)
cq.exporters.export(t, str(HERE / "bevelB_tray.stl"), tolerance=0.02, angularTolerance=0.2)
print("valid", t.val().isValid(), "solids", len(t.solids().vals()),
      "tray mm3", round(tray.val().Volume()), "->", round(t.val().Volume()))
