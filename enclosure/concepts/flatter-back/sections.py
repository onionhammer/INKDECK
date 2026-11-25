# /// script
# requires-python = ">=3.10"
# dependencies = ["trimesh", "numpy", "matplotlib", "shapely", "networkx", "scipy", "rtree"]
# ///
"""Sections through v0.5 and A, with the board, battery and placeholder speaker: along the length at
y 15 and across at x 44.8 (through an MX1.25 header). Writes sections.png. Run depthmap.py (for
build/board.stl) and build_pod.py first.

    uv run enclosure/concepts/flatter-back/sections.py
"""
from pathlib import Path
import numpy as np, trimesh, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from shapely.geometry import Polygon

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
M = {k: trimesh.load(BUILD / f"{k}.stl") for k in ["v05_tray", "v05_bezel", "battery", "speaker", "board"]}
M |= {k: trimesh.load(HERE / f"{k}.stl") for k in ["podA_tray", "podA_bezel"]}
STY = {"tray": "#b5561f", "bezel": "#3b4049", "battery": "#4a8fd4", "speaker": "#d4b84a", "board": "#2f7d4a"}

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
o1, n1 = [0, 15.0, 0], [0, 1, 0]
panel(axs[0], "v05_tray", "v05_bezel", o1, n1, [0, 2], "v0.5 — section along the length at y 15 (battery, and where a speaker could go)", False)
panel(axs[1], "podA_tray", "podA_bezel", o1, n1, [0, 2], "A · pod back — same section: the back hugs the battery; a 20×15×3.6 placeholder speaker sits on the field over a grille", True)
o2, n2 = [44.8, 0, 0], [1, 0, 0]
panel(axs[2], "v05_tray", "v05_bezel", o2, n2, [1, 2], "v0.5 — section across at x 44.8 (an MX1.25 header on the left, the battery)", False)
panel(axs[3], "podA_tray", "podA_bezel", o2, n2, [1, 2], "A · pod back — same section", False)
fig.legend(handles=[Patch(color=c, label=k) for k, c in STY.items()], loc="lower center", ncol=5, frameon=False)
plt.tight_layout(rect=(0, 0.03, 1, 1)); plt.savefig(HERE / "sections.png", dpi=110)
