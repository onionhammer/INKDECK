# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4", "numpy", "matplotlib", "scipy"]
# ///
"""How deep the board reaches below the PCB, everywhere: rasterises every part of Waveshare's board
STEP that hangs below the PCB into a min-z map (0.25 mm grid, enclosure frame) and plots it as
../docs/img/depthmap.png, with the battery and the speaker on it. Also writes ../build/board.stl
for pictures.py and ../build/depth.npz for build.py.

    uv run enclosure/tools/depthmap.py
"""
from pathlib import Path
import numpy as np
import cadquery as cq
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from scipy import ndimage
from OCP.STEPCAFControl import STEPCAFControl_Reader
from OCP.TDocStd import TDocStd_Document
from OCP.XCAFDoc import XCAFDoc_DocumentTool
from OCP.TDF import TDF_LabelSequence, TDF_Label
from OCP.TCollection import TCollection_ExtendedString
from OCP.TDataStd import TDataStd_Name
from OCP.TopLoc import TopLoc_Location
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE
from OCP.BRep import BRep_Tool
from OCP.TopoDS import TopoDS

ENC = Path(__file__).resolve().parents[1]   # enclosure/
BUILD = ENC / "build"; BUILD.mkdir(exist_ok=True)
BOARD = ENC.parent / "docs/mechanical/ESP32-S3-EPAPER-3_97-20260821.stp"

doc = TDocStd_Document(TCollection_ExtendedString("doc"))
r = STEPCAFControl_Reader(); r.SetNameMode(True); r.ReadFile(str(BOARD)); r.Transfer(doc)
st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
leaves = []
def nm(lab):
    n = TDataStd_Name()
    return n.Get().ToExtString() if lab.FindAttribute(TDataStd_Name.GetID_s(), n) else "?"
def walk(lab, loc):
    if st.IsAssembly_s(lab):
        comps = TDF_LabelSequence(); st.GetComponents_s(lab, comps)
        for i in range(1, comps.Length()+1):
            c = comps.Value(i); ref = TDF_Label(); st.GetReferredShape_s(c, ref)
            walk(ref, loc.Multiplied(st.GetLocation_s(c)))
    else:
        leaves.append((nm(lab), st.GetShape_s(lab).Moved(loc)))
roots = TDF_LabelSequence(); st.GetFreeShapes(roots)
for i in range(1, roots.Length()+1): walk(roots.Value(i), TopLoc_Location())

G = 0.25; X0, X1, Y0, Y1 = -3.0, 103.0, -3.0, 64.0
nx, ny = int((X1-X0)/G), int((Y1-Y0)/G)
zmin = np.full((ny, nx), np.inf)
xs = X0 + (np.arange(nx)+0.5)*G; ys = Y0 + (np.arange(ny)+0.5)*G
for name, shp in leaves:
    b = Bnd_Box(); BRepBndLib.Add_s(shp, b, True)
    if b.Get()[2] > -1.61 or name in ("BOARD", "EVA"): continue   # nothing below the PCB, or the PCB itself
    BRepMesh_IncrementalMesh(shp, 0.05, False, 0.3, True)
    ex = TopExp_Explorer(shp, TopAbs_FACE)
    while ex.More():
        f = TopoDS.Face_s(ex.Current()); loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(f, loc)
        if tri is not None:
            tr = loc.Transformation()
            P = np.array([[*(lambda p: (p.X(), p.Y(), p.Z()))(tri.Node(i).Transformed(tr))] for i in range(1, tri.NbNodes()+1)])
            for k in range(1, tri.NbTriangles()+1):
                a, bb, c = tri.Triangle(k).Get()
                A, B, C = P[a-1], P[bb-1], P[c-1]
                lo = np.minimum(np.minimum(A, B), C); hi = np.maximum(np.maximum(A, B), C)
                i0 = max(int((lo[0]-X0)/G)-1, 0); i1 = min(int((hi[0]-X0)/G)+2, nx)
                j0 = max(int((lo[1]-Y0)/G)-1, 0); j1 = min(int((hi[1]-Y0)/G)+2, ny)
                if i0 >= i1 or j0 >= j1: continue
                gx, gy = np.meshgrid(xs[i0:i1], ys[j0:j1])
                d = (B[1]-C[1])*(A[0]-C[0]) + (C[0]-B[0])*(A[1]-C[1])
                if abs(d) < 1e-12:  # vertical triangle: stamp its min z on its xy segment cells
                    m = (gx >= lo[0]-G/2) & (gx <= hi[0]+G/2) & (gy >= lo[1]-G/2) & (gy <= hi[1]+G/2)
                    sub = zmin[j0:j1, i0:i1]; sub[m] = np.minimum(sub[m], lo[2]); continue
                l1 = ((B[1]-C[1])*(gx-C[0]) + (C[0]-B[0])*(gy-C[1]))/d
                l2 = ((C[1]-A[1])*(gx-C[0]) + (A[0]-C[0])*(gy-C[1]))/d
                l3 = 1-l1-l2
                m = (l1 >= -0.02) & (l2 >= -0.02) & (l3 >= -0.02)
                z = l1*A[2] + l2*B[2] + l3*C[2]
                sub = zmin[j0:j1, i0:i1]; sub[m] = np.minimum(sub[m], z[m])
        ex.Next()
np.savez(BUILD / "depth.npz", zmin=zmin, xs=xs, ys=ys)
print("deepest part below the PCB: z", round(float(zmin[np.isfinite(zmin)].min()), 2))

z = np.where(np.isfinite(zmin), zmin, -1.6)
z = ndimage.minimum_filter(z, size=3)   # close the rasteriser's hairline gaps between a part's triangles
fig, ax = plt.subplots(figsize=(13, 8))
im = ax.imshow(z, origin="lower", extent=[xs[0]-G/2, xs[-1]+G/2, ys[0]-G/2, ys[-1]+G/2], cmap="viridis_r", vmin=-5.7, vmax=-1.6)
plt.colorbar(im, label="lowest part under the PCB, z mm (PCB bottom −1.6)")
ax.add_patch(Rectangle((-2.4, -2.25), 104.3, 64.5, fill=False, ec="w", lw=1.5, ls="--"))
ax.add_patch(Rectangle((7.8, 8.6), 65.0, 45.0, fill=False, ec="r", lw=2))     # build.py's BATT
ax.add_patch(Rectangle((7.8, 7.6), 65.0, 47.0, fill=False, ec="r", lw=1, ls=":"))  # and its POCKET
ax.text(40.3, 31.1, "battery about 65×45×4.5\n(wiring room in), room to −3.7\npocket 65×47 dotted", color="r", ha="center", va="center", fontsize=11, weight="bold")
# the speaker (build.py's SPK)
ax.add_patch(Rectangle((85.4, 7.8), 14.0, 10.0, fill=False, ec="#ffb000", lw=2))
ax.text(92.4, 12.8, "speaker\n14×10", color="#ffb000", ha="center", va="center", fontsize=9, weight="bold")
for x in np.arange(0, 104, 10): ax.axvline(x, color="w", lw=0.2)
for y in np.arange(0, 64, 10): ax.axhline(y, color="w", lw=0.2)
ax.set_title("Depth map under the board (board STEP), enclosure frame")
ax.set_xlabel("x"); ax.set_ylabel("y")
plt.savefig(ENC / "docs/img/depthmap.png", dpi=110, bbox_inches="tight")

cq.exporters.export(cq.importers.importStep(str(BOARD)), str(BUILD / "board.stl"), tolerance=0.05, angularTolerance=0.3)
