# /// script
# requires-python = ">=3.10"
# dependencies = ["trimesh", "numpy", "matplotlib", "shapely", "networkx", "scipy", "rtree"]
# ///
"""Sections through v0.6 and A, with the board, battery and placeholder speaker: along the length at
y 13.5 and across at x 44.8 (through an MX1.25 header). Writes sections.png. Then close-ups of A2's
seam, latches and bevels as a2_seam.png. Run depthmap.py (for build/board.stl), build_pod.py and
build_a2.py first.

    uv run enclosure/concepts/flatter-back/sections.py
"""
from pathlib import Path
import numpy as np, trimesh, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from shapely.geometry import Polygon

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
M = {k: trimesh.load(BUILD / f"{k}.stl") for k in ["v06_tray", "v06_bezel", "battery", "speaker", "board"]}
M |= {k: trimesh.load(HERE / f"{k}.stl") for k in ["podA_tray", "podA_bezel", "a2_tray", "a2_tub", "a2_bezel"]}
STY = {"tray": "#b5561f", "bezel": "#3b4049", "battery": "#4a8fd4", "speaker": "#d4b84a", "board": "#2f7d4a"}
TUB = "#e08a4c"

def polys(mesh, origin, normal, axes):
    s = mesh.section(plane_origin=origin, plane_normal=normal)
    if s is None: return []
    out = []
    for e in s.discrete:
        p = e[:, axes]
        if len(p) >= 3:
            g = Polygon(p).buffer(0)
            if not g.is_empty: out.append(g)
    if not out: return []
    acc = out[0]                       # even-odd fill: xor all loops
    for g in out[1:]: acc = acc.symmetric_difference(g)
    return [acc]

def draw(ax, geoms, color, alpha=1.0, z=1):
    for g in geoms:
        for p in getattr(g, "geoms", [g]):
            if p.is_empty or p.geom_type != "Polygon": continue
            x, y = p.exterior.xy; ax.fill(x, y, color=color, alpha=alpha, lw=0, zorder=z)
            for h in p.interiors:
                x, y = h.xy; ax.fill(x, y, color="white", lw=0, zorder=z)

def panel(ax, tray, bezel, origin, normal, axes, title, show_spk):
    draw(ax, polys(M["board"], origin, normal, axes), STY["board"], 0.9, 2)
    draw(ax, polys(M["battery"], origin, normal, axes), STY["battery"], 0.75, 2)
    if show_spk: draw(ax, polys(M["speaker"], origin, normal, axes), STY["speaker"], 0.85, 2)
    draw(ax, polys(M[tray], origin, normal, axes), STY["tray"], 1, 3)
    draw(ax, polys(M[bezel], origin, normal, axes), STY["bezel"], 1, 3)
    ax.set_aspect("equal"); ax.set_title(title, loc="left", fontsize=10)
    ax.axhline(-9.11, color="#999", lw=0.6, ls=":"); ax.axhline(-7.2, color="#999", lw=0.6, ls=":")
    ax.tick_params(labelsize=8); ax.set_ylim(-10, 3.5); ax.set_xlim(-4, 104)
    ax.text(103.5, -9.11, "z −9.11", fontsize=7, va="center", ha="right", color="#666")
    ax.text(103.5, -7.2, "z −7.2", fontsize=7, va="bottom", ha="right", color="#666")

fig, axs = plt.subplots(4, 1, figsize=(15, 11.5))
o1, n1 = [0, 13.5, 0], [0, 1, 0]
panel(axs[0], "v06_tray", "v06_bezel", o1, n1, [0, 2], "v0.6 — section along the length at y 13.5 (battery, and where the speaker goes)", False)
panel(axs[1], "podA_tray", "podA_bezel", o1, n1, [0, 2], "A · pod back — same section: the back hugs the battery; the 14×10 speaker (3.0 drawn) sits on the field over its grille", True)
o2, n2 = [44.8, 0, 0], [1, 0, 0]
panel(axs[2], "v06_tray", "v06_bezel", o2, n2, [1, 2], "v0.6 — section across at x 44.8 (an MX1.25 header on the left, the battery)", False)
panel(axs[3], "podA_tray", "podA_bezel", o2, n2, [1, 2], "A · pod back — same section", False)
fig.legend(handles=[Patch(color=c, label=k) for k, c in STY.items()], loc="lower center", ncol=5, frameon=False)
plt.tight_layout(rect=(0, 0.03, 1, 1)); plt.savefig(HERE / "sections.png", dpi=110)

# A2 close-ups, full height: the 45° seat, the latches and the bevels
def zoom(ax, v, origin, normal, axes, xlim, title, notes):
    draw(ax, polys(M["board"], origin, normal, axes), STY["board"], 0.9, 2)
    draw(ax, polys(M["battery"], origin, normal, axes), STY["battery"], 0.75, 2)
    draw(ax, polys(M[f"{v}_tray"], origin, normal, axes), STY["tray"], 1, 3)
    draw(ax, polys(M["a2_tub"], origin, normal, axes), TUB, 1, 3)
    draw(ax, polys(M[f"{v}_bezel"], origin, normal, axes), STY["bezel"], 1, 3)
    ax.set_aspect("equal"); ax.set_xlim(*xlim); ax.set_ylim(-9.6, 3.4)
    ax.set_title(title, loc="left", fontsize=10); ax.tick_params(labelsize=8)
    for z in (-9.11, -7.2, -2.6): ax.axhline(z, color="#999", lw=0.6, ls=":")
    for (x, y), (tx, ty), text in notes:
        ax.annotate(text, (x, y), (tx, ty), fontsize=8, arrowprops=dict(arrowstyle="-", color="#333", lw=0.6))

fig, axs = plt.subplots(2, 2, figsize=(15, 9.6))
X, Y, L, R = [1, 0, 0], [0, 1, 0], (-3.6, 16.4), (44.6, 64.6)
zoom(axs[0, 0], "a2", [39.85, 0, 0], X, [1, 2], L, "A2 · across at x 39.85, PWR side: a latch, and 0.8 bevels back and front",
     [((-1.85, -6.8), (-3.2, -8.9), "0.8 × 45°"), ((-1.85, 2.35), (1.0, 3.0), "0.8 × 45° (A: 0.5)"),
      ((10.0, -6.3), (1.5, -4.6), "45° seat (0.6)"), ((9.9, -5.4), (13.0, -1.0), "barb, 45° both faces"),
      ((11.0, -6.2), (12.6, -8.9), "slit under the beam")])
zoom(axs[0, 1], "a2", [39.85, 0, 0], X, [1, 2], R, "A2 · same section, thumbwheel side: the other latch",
     [((63.05, -6.8), (60.5, -8.9), "0.8 × 45°"), ((61.85, 2.35), (56.0, 3.0), "0.8 × 45°"), ((57.7, -5.4), (52.5, -1.0), "barb")])
zoom(axs[1, 0], "a2", [20.0, 0, 0], X, [1, 2], L, "A2 · across at x 20, no latch: the tub sits on the ledge",
     [((10.0, -6.3), (1.5, -4.6), "45° seat"), ((9.78, -7.2), (3.0, -8.9), "seam, 0.15")])
zoom(axs[1, 1], "a2", [0, 30.0, 0], Y, [0, 2], L, "A2 · along the length at y 30: the USB-C end",
     [((-1.95, -6.8), (-3.2, -8.9), "0.4 left under the opening"), ((5.6, -6.3), (8.0, -1.0), "45° seat")])
fig.legend(handles=[Patch(color=c, label=k) for k, c in [("tray", STY["tray"]), ("tub", TUB), ("top cover", STY["bezel"]),
                                                        ("battery", STY["battery"]), ("board", STY["board"])]],
           loc="lower center", ncol=5, frameon=False)
plt.tight_layout(rect=(0, 0.03, 1, 1)); plt.savefig(HERE / "a2_seam.png", dpi=110)
