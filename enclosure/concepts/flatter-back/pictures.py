# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["pyvista>=0.44", "numpy", "pillow", "trimesh", "matplotlib", "shapely", "networkx", "scipy", "rtree"]
# ///
"""The pictures, from build.py's parts and depthmap.py's board (run both first):

- sheet.png: v0.6 and the flatter back, a back view, a side and an end elevation
- parts.png: the parts exploded, and as they lie on the bed
- sections.png: v0.6 and the flatter back cut along the length at y 13.5 (battery and speaker) and
  across at x 44.8 (an MX1.25 header), with the board, battery and speaker
- seam.png: close-ups of the tub's seat, the latches and the bevels

    uv run enclosure/concepts/flatter-back/pictures.py
"""
from pathlib import Path
import pyvista as pv, trimesh, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from shapely.geometry import Polygon
from PIL import Image

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
TRAY, TUB, BEZEL, BG = "#b5561f", "#c9692c", "#3b4049", "#2d3035"   # the tub a shade lighter, to show the split
TUB_2D, BATTERY, SPEAKER, BOARD = "#e08a4c", "#4a8fd4", "#d4b84a", "#2f7d4a"
V06 = [(BUILD / "v06_tray.stl", TRAY), (BUILD / "v06_bezel.stl", BEZEL)]
NEW = [(HERE / "back-tray.stl", TRAY), (HERE / "battery-tub.stl", TUB), (HERE / "front-bezel.stl", BEZEL)]

# --- renders -----------------------------------------------------------------------------------------
pv.OFF_SCREEN = True
C = (49.75, 30.0, -3.2)
SETS = [(V06, "v0.6 — flat back, 11.86"), (NEW, "flatter back — 11.86 at the battery tub, 9.95 elsewhere")]

def shot(out, cam, size, parallel=None, rows=False):
    pl = pv.Plotter(shape=(len(SETS), 1) if rows else (1, len(SETS)), window_size=size, off_screen=True, border=False)
    for i, (parts, title) in enumerate(SETS):
        pl.subplot(*((i, 0) if rows else (0, i))); pl.set_background(BG)
        for path, color in parts:
            pl.add_mesh(pv.read(path), color=color, specular=0.15, ambient=0.28, diffuse=0.8)
        pl.camera_position = cam
        if parallel:
            pl.enable_parallel_projection(); pl.camera.parallel_scale = parallel
        pl.add_text(title, position="upper_left", font_size=11 if rows else 12, color="white")
    pl.screenshot(str(out)); pl.close()

W = 1200 * len(SETS)
shot(BUILD / "cmp_back_iso.png", [(C[0]+95, C[1]-150, C[2]-120), C, (0, 0, 1)], (W, 1140))
shot(BUILD / "cmp_side.png", [(C[0], C[1]-300, C[2]), C, (0, 0, 1)], (1500, 260 * len(SETS)), parallel=10.6, rows=True)
shot(BUILD / "cmp_end.png", [(C[0]+300, C[1], C[2]), C, (0, 0, 1)], (1100, 260 * len(SETS)), parallel=10.6, rows=True)
# the sheet: back views on top, the side (USB-C end on the left) and the antenna end below them
a = Image.open(BUILD / "cmp_back_iso.png").crop((0, 0, W, 885))
b = Image.open(BUILD / "cmp_side.png"); c = Image.open(BUILD / "cmp_end.png")
bw = W * 7 // 12; b = b.resize((bw, int(b.height * bw / b.width)))
cw = W - bw; c = c.resize((cw, int(c.height * cw / c.width)))
row2 = Image.new("RGB", (W, max(b.height, c.height)), BG); row2.paste(b, (0, 0)); row2.paste(c, (bw, 0))
sheet = Image.new("RGB", (W, a.height + row2.height), BG); sheet.paste(a, (0, 0)); sheet.paste(row2, (0, a.height))
sheet.save(HERE / "sheet.png")

# the parts: exploded from the back (the tub goes in from outside), and as they print, on one bed
tray, tub, bezel = (pv.read(p) for p, _ in NEW)
pl = pv.Plotter(shape=(1, 2), window_size=(2400, 900), off_screen=True, border=False)
pl.subplot(0, 0); pl.set_background(BG)
for m, col, dz in ((bezel, BEZEL, 0), (tray, TRAY, 0), (tub, TUB, -16)):
    pl.add_mesh(m.translate((0, 0, dz)), color=col, specular=0.15, ambient=0.28, diffuse=0.8)
pl.camera_position = [(C[0]+110, C[1]-170, C[2]-150), (C[0], C[1], C[2]-7), (0, 0, 1)]
pl.add_text("Exploded, from the back: the tub goes in from outside", position="upper_left", font_size=12, color="white")
pl.subplot(0, 1); pl.set_background(BG)
pl.add_mesh(pv.Plane(center=(49.75, 62, -7.21), direction=(0, 0, 1), i_size=150, j_size=160), color="#55595f", ambient=0.4)
pl.add_mesh(tray, color=TRAY, specular=0.15, ambient=0.28, diffuse=0.8)
pl.add_mesh(tub.translate((0, 70, -7.2 - tub.bounds[4])), color=TUB, specular=0.15, ambient=0.28, diffuse=0.8)
pl.camera_position = [(49.75 - 60, 62 - 150, 120), (49.75, 62, -6), (0, 0, 1)]
pl.add_text("As printed: tray on its back, tub on its pod face. No supports", position="upper_left", font_size=12, color="white")
pl.screenshot(str(HERE / "parts.png")); pl.close()

# --- sections ----------------------------------------------------------------------------------------
M = {p.stem: trimesh.load(p) for p, _ in V06 + NEW}
M |= {k: trimesh.load(BUILD / f"{k}.stl") for k in ("battery", "speaker", "board")}
FLAT = {TRAY: TRAY, TUB: TUB_2D, BEZEL: BEZEL}
SHOW = {"v0.6": [(p.stem, FLAT[c]) for p, c in V06], "flatter back": [(p.stem, FLAT[c]) for p, c in NEW]}

def polys(mesh, origin, normal, axes):
    s = mesh.section(plane_origin=origin, plane_normal=normal)
    if s is None: return []
    loops = [Polygon(e[:, axes]).buffer(0) for e in s.discrete if len(e) >= 3]
    loops = [g for g in loops if not g.is_empty]
    if not loops: return []
    acc = loops[0]                       # even-odd fill: xor all loops
    for g in loops[1:]: acc = acc.symmetric_difference(g)
    return [acc]

def draw(ax, geoms, color, alpha=1.0, z=1):
    for g in geoms:
        for p in getattr(g, "geoms", [g]):
            if p.is_empty or p.geom_type != "Polygon": continue
            x, y = p.exterior.xy; ax.fill(x, y, color=color, alpha=alpha, lw=0, zorder=z)
            for h in p.interiors:
                x, y = h.xy; ax.fill(x, y, color="white", lw=0, zorder=z)

def cut(ax, case, origin, normal, axes, speaker=True):
    draw(ax, polys(M["board"], origin, normal, axes), BOARD, 0.9, 2)
    draw(ax, polys(M["battery"], origin, normal, axes), BATTERY, 0.75, 2)
    if speaker: draw(ax, polys(M["speaker"], origin, normal, axes), SPEAKER, 0.85, 2)
    for key, color in SHOW[case]: draw(ax, polys(M[key], origin, normal, axes), color, 1, 3)
    ax.set_aspect("equal"); ax.tick_params(labelsize=8)

def legend(fig):
    fig.legend(handles=[Patch(color=c, label=k) for k, c in [("tray", TRAY), ("tub", TUB_2D), ("front bezel", BEZEL),
               ("battery", BATTERY), ("speaker", SPEAKER), ("board", BOARD)]], loc="lower center", ncol=6, frameon=False)

fig, axs = plt.subplots(4, 1, figsize=(15, 11.5))
for ax, case, o, n, axes, title in [
        (axs[0], "v0.6", [0, 13.5, 0], [0, 1, 0], [0, 2], "v0.6 — along the length at y 13.5: the battery, and where the speaker goes"),
        (axs[1], "flatter back", [0, 13.5, 0], [0, 1, 0], [0, 2], "flatter back — same section: the back hugs the battery; the 14×10 speaker (3.0 drawn) sits on the floor over its grille"),
        (axs[2], "v0.6", [44.8, 0, 0], [1, 0, 0], [1, 2], "v0.6 — across at x 44.8: an MX1.25 header on the left, the battery"),
        (axs[3], "flatter back", [44.8, 0, 0], [1, 0, 0], [1, 2], "flatter back — same section")]:
    cut(ax, case, o, n, axes, speaker=case != "v0.6")
    ax.set_title(title, loc="left", fontsize=10); ax.set_ylim(-10, 3.5); ax.set_xlim(-4, 104)
    for z in (-9.11, -7.2):
        ax.axhline(z, color="#999", lw=0.6, ls=":"); ax.text(103.5, z, f"z {z:.2f}".replace("-", "−"), fontsize=7, va="bottom", ha="right", color="#666")
legend(fig); plt.tight_layout(rect=(0, 0.03, 1, 1)); plt.savefig(HERE / "sections.png", dpi=110)

def zoom(ax, origin, normal, axes, xlim, title, notes):
    cut(ax, "flatter back", origin, normal, axes, speaker=False)
    ax.set_xlim(*xlim); ax.set_ylim(-9.6, 3.4); ax.set_title(title, loc="left", fontsize=10)
    for z in (-9.11, -7.2, -2.6): ax.axhline(z, color="#999", lw=0.6, ls=":")
    for (x, y), (tx, ty), text in notes:
        ax.annotate(text, (x, y), (tx, ty), fontsize=8, arrowprops=dict(arrowstyle="-", color="#333", lw=0.6))

fig, axs = plt.subplots(2, 2, figsize=(15, 9.6))
X, Y, L, R = [1, 0, 0], [0, 1, 0], (-3.6, 16.4), (44.6, 64.6)
zoom(axs[0, 0], [39.85, 0, 0], X, [1, 2], L, "across at x 39.85, PWR side: a latch, and the 0.8 bevels back and front",
     [((-1.85, -6.8), (-3.2, -8.9), "0.8 × 45°"), ((-1.85, 2.35), (1.0, 3.0), "0.8 × 45° (v0.6: 0.5)"),
      ((10.0, -6.3), (1.5, -4.6), "45° seat (0.6)"), ((9.9, -5.4), (13.0, -1.0), "barb, 45° both faces"),
      ((11.0, -6.2), (12.6, -8.9), "slit under the beam")])
zoom(axs[0, 1], [39.85, 0, 0], X, [1, 2], R, "same section, thumbwheel side: the other latch",
     [((63.05, -6.8), (60.5, -8.9), "0.8 × 45°"), ((61.85, 2.35), (56.0, 3.0), "0.8 × 45°"), ((57.7, -5.4), (52.5, -1.0), "barb")])
zoom(axs[1, 0], [20.0, 0, 0], X, [1, 2], L, "across at x 20, no latch: the tub sits on the ledge",
     [((10.0, -6.3), (1.5, -4.6), "45° seat"), ((9.78, -7.2), (3.0, -8.9), "seam, 0.15")])
zoom(axs[1, 1], [0, 30.0, 0], Y, [0, 2], L, "along the length at y 30: the USB-C end",
     [((-1.95, -6.8), (-3.2, -8.9), "0.4 left under the opening"), ((5.6, -6.3), (8.0, -1.0), "45° seat")])
legend(fig); plt.tight_layout(rect=(0, 0.03, 1, 1)); plt.savefig(HERE / "seam.png", dpi=110)
