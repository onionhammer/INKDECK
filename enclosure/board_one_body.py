# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "shapely>=2.0", "numpy"]
# ///
"""Waveshare's board STEP as one light body, for ScanRuler: one body to show, hide or cut with the
enclosure, in the frame the file was drawn in. Writes board-one-body.step here.

    uv run enclosure/board_one_body.py

The file's 678 solids fused as they are come to 64,000 faces, a 329 MB STEP, and still 76 pieces
(parts that stand a hair off the board). So this is a stand-in: the PCB, the panel and the standoffs
are kept as drawn, and every other part is its outline, seen square to the board, carried from its
far face to the board. Each part keeps its footprint and its height and loses its detail, and is
never smaller than the real one, so a clearance read off it is on the safe side.

In ScanRuler: Import STEP (reverse.import.step) with board-one-body.step.
"""
from pathlib import Path
import math, time
import cadquery as cq
from shapely.geometry import Polygon
from shapely.ops import unary_union
from OCP.STEPCAFControl import STEPCAFControl_Reader
from OCP.TDocStd import TDocStd_Document
from OCP.XCAFDoc import XCAFDoc_DocumentTool
from OCP.TDF import TDF_LabelSequence, TDF_Label
from OCP.TCollection import TCollection_ExtendedString
from OCP.TDataStd import TDataStd_Name
from OCP.TopLoc import TopLoc_Location
from OCP.Interface import Interface_Static

HERE = Path(__file__).resolve().parent
BOARD = HERE.parent / "docs/mechanical/ESP32-S3-EPAPER-3_97-20260821.stp"
OUT = HERE / "board-one-body.step"
PCB = (-1.6, 0.0)                        # the PCB's faces, z
KEEP = ("BOARD", "3_97-E_PAPER", "STUDS")  # kept as drawn (names start with these)
INTO = 0.05                              # a part's outline runs this far into the PCB, so the two join
CLOSE = 0.1                              # gaps under 0.2 in an outline (between pins) are closed
TOL = 0.02                               # outlines are read and simplified to this

t0 = time.time()
doc = TDocStd_Document(TCollection_ExtendedString("doc"))
rd = STEPCAFControl_Reader(); rd.SetNameMode(True); rd.ReadFile(str(BOARD)); rd.Transfer(doc)
tool = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
def name_of(lab):
    n = TDataStd_Name()
    return n.Get().ToExtString() if lab.FindAttribute(TDataStd_Name.GetID_s(), n) else "?"
leaves = []
def walk(lab, loc):
    if tool.IsAssembly_s(lab):
        comps = TDF_LabelSequence(); tool.GetComponents_s(lab, comps)
        for i in range(1, comps.Length() + 1):
            c = comps.Value(i); ref = TDF_Label(); tool.GetReferredShape_s(c, ref)
            walk(ref, loc.Multiplied(tool.GetLocation_s(c)))
    else:
        leaves.extend((name_of(lab), s) for s in cq.Shape.cast(tool.GetShape_s(lab).Moved(loc)).Solids())
roots = TDF_LabelSequence(); tool.GetFreeShapes(roots)
for i in range(1, roots.Length() + 1): walk(roots.Value(i), TopLoc_Location())
print(f"{len(leaves)} solids, {sum(len(s.Faces()) for _, s in leaves)} faces, read in {time.time() - t0:.0f} s", flush=True)

def outline(solid):
    """The solid's outline seen along z: polygons, their holes filled."""
    verts, tris = solid.tessellate(TOL, 0.3)
    flat = [Polygon([(verts[i].x, verts[i].y) for i in tri]) for tri in tris]
    u = unary_union([p for p in flat if p.area > 1e-9])
    u = u.buffer(CLOSE, join_style=2).buffer(-CLOSE, join_style=2).simplify(TOL)
    return [Polygon(g.exterior) for g in getattr(u, "geoms", [u]) if g.area > 0.01]

def prism(poly, z0, z1):
    x0, y0, x1, y1 = poly.bounds
    r = (x1 - x0) / 2
    if abs((x1 - x0) - (y1 - y0)) < 2 * TOL and poly.area > 0.985 * math.pi * r * r:        # a round one
        return cq.Workplane("XY", origin=((x0 + x1) / 2, (y0 + y1) / 2, z0)).circle(r).extrude(z1 - z0).val()
    return cq.Workplane("XY", origin=(0, 0, z0)).polyline(list(poly.exterior.coords)[:-1]).close().extrude(z1 - z0).val()

kept, made = [], []
for name, s in leaves:
    if name.startswith(KEEP): kept.append(s); continue
    b = s.BoundingBox(); z0, z1 = b.zmin, b.zmax
    if z1 < PCB[0] + 0.35: z1 = PCB[0] + INTO          # under the board: up to it
    if z0 > PCB[1] - 0.35: z0 = PCB[1] - INTO          # over it: down to it
    made += [prism(p, z0, z1) for p in outline(s)]
print(f"{len(kept)} kept as drawn, {len(made)} outlines, {time.time() - t0:.0f} s", flush=True)

kept.sort(key=lambda s: -s.Volume())
body = kept[0].fuse(*kept[1:], *made, tol=1e-4)
try:
    body = body.clean()          # join the faces that lie in one surface
except Exception as e:
    print("not tidied:", e)
parts = body.Solids()
full = sum(s.Volume() for _, s in leaves)
print(f"{len(parts)} solid(s), {len(body.Faces())} faces, valid {body.isValid()}, "
      f"volume {body.Volume():.0f} mm3 (the parts as drawn: {full:.0f})")
if len(parts) > 1:
    for s in sorted(parts, key=lambda s: -s.Volume())[1:9]:
        b = s.BoundingBox()
        print(f"   apart: {s.Volume():8.2f} mm3  x {b.xmin:6.1f}..{b.xmax:6.1f}  y {b.ymin:6.1f}..{b.ymax:6.1f}  z {b.zmin:6.2f}..{b.zmax:6.2f}")
Interface_Static.SetIVal_s("write.surfacecurve.mode", 0)     # no curves on the surfaces: half the file, and readers make their own
cq.exporters.export(cq.Workplane().add(body), str(OUT))
print(f"wrote {OUT.name}, {OUT.stat().st_size / 1e6:.1f} MB, {time.time() - t0:.0f} s in all")
