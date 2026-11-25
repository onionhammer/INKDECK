# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["pyvista>=0.44", "numpy", "pillow"]
# ///
"""v0.6, A, B and A2 side by side: a back view, a side and an end elevation, put together as
concepts_sheet.png. Then A2's parts, exploded and as they lie on the bed, as a2_parts.png. Run
build_pod.py, build_bevel.py and build_a2.py first.

    uv run enclosure/concepts/flatter-back/render.py
"""
from pathlib import Path
import pyvista as pv
from PIL import Image

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
pv.OFF_SCREEN = True
TRAY, TUB, BEZEL, BG = "#b5561f", "#c9692c", "#3b4049", "#2d3035"   # the tub a shade lighter, to show the split
C = (49.75, 30.0, -3.2)
SETS = [([(BUILD / "v06_tray.stl", TRAY), (BUILD / "v06_bezel.stl", BEZEL)], "v0.6 — flat back, 11.86"),
        ([(HERE / "podA_tray.stl", TRAY), (HERE / "podA_bezel.stl", BEZEL)], "A · pod back — 11.86 at the pod, 9.95 elsewhere"),
        ([(HERE / "bevelB_tray.stl", TRAY), (BUILD / "v06_bezel.stl", BEZEL)], "B · bevelled edge — 11.86, edges read 9.86"),
        ([(HERE / "a2_tray.stl", TRAY), (HERE / "a2_tub.stl", TUB), (HERE / "a2_bezel.stl", BEZEL)],
         "A2 · A in 3 parts, 0.8 bevels back and front")]
W = 800 * len(SETS)

def shot(out, cam, size, parallel=None, rows=False):
    shape = (len(SETS), 1) if rows else (1, len(SETS))
    pl = pv.Plotter(shape=shape, window_size=size, off_screen=True, border=False)
    for i, (parts, title) in enumerate(SETS):
        pl.subplot(*((i, 0) if rows else (0, i))); pl.set_background(BG)
        for path, color in parts:
            pl.add_mesh(pv.read(path), color=color, specular=0.15, ambient=0.28, diffuse=0.8)
        pl.camera_position = cam
        if parallel:
            pl.enable_parallel_projection(); pl.camera.parallel_scale = parallel
        pl.add_text(title, position="upper_left", font_size=11 if rows else 12, color="white")
    pl.screenshot(str(out)); pl.close()

n = len(SETS)
shot(BUILD / "cmp_back_iso.png", [(C[0]+95, C[1]-150, C[2]-120), C, (0, 0, 1)], (800 * n, 760))
shot(BUILD / "cmp_side.png", [(C[0], C[1]-300, C[2]), C, (0, 0, 1)], (1500, 260 * n), parallel=10.6, rows=True)
shot(BUILD / "cmp_end.png", [(C[0]+300, C[1], C[2]), C, (0, 0, 1)], (1100, 260 * n), parallel=10.6, rows=True)

# the sheet: back view on top, the side (USB-C end on the left) and the antenna end below it
a = Image.open(BUILD / "cmp_back_iso.png").crop((0, 0, W, 590))
b = Image.open(BUILD / "cmp_side.png"); c = Image.open(BUILD / "cmp_end.png")
bw = W * 7 // 12; b = b.resize((bw, int(b.height * bw / b.width)))
cw = W - bw; c = c.resize((cw, int(c.height * cw / c.width)))
row2 = Image.new("RGB", (W, max(b.height, c.height)), BG); row2.paste(b, (0, 0)); row2.paste(c, (bw, 0))
sheet = Image.new("RGB", (W, a.height + row2.height), BG); sheet.paste(a, (0, 0)); sheet.paste(row2, (0, a.height))
sheet.save(HERE / "concepts_sheet.png"); print(sheet.size)

# A2's parts: exploded from the back (the tub goes in from outside), and as they print, on one bed
tray, tub, bezel = (pv.read(HERE / f"a2_{k}.stl") for k in ("tray", "tub", "bezel"))
pl = pv.Plotter(shape=(1, 2), window_size=(2400, 900), off_screen=True, border=False)
pl.subplot(0, 0); pl.set_background(BG)
for m, col, dz in ((bezel, BEZEL, 0), (tray, TRAY, 0), (tub, TUB, -16)):
    pl.add_mesh(m.translate((0, 0, dz)), color=col, specular=0.15, ambient=0.28, diffuse=0.8)
pl.camera_position = [(C[0]+110, C[1]-170, C[2]-150), (C[0], C[1], C[2]-7), (0, 0, 1)]
pl.add_text("A2 exploded, from the back: the tub goes in from outside", position="upper_left", font_size=12, color="white")
pl.subplot(0, 1); pl.set_background(BG)
bed = pv.Plane(center=(49.75, 62, -7.21), direction=(0, 0, 1), i_size=150, j_size=160)
pl.add_mesh(bed, color="#55595f", ambient=0.4)
pl.add_mesh(tray, color=TRAY, specular=0.15, ambient=0.28, diffuse=0.8)
pl.add_mesh(tub.translate((0, 70, -7.2 - tub.bounds[4])), color=TUB, specular=0.15, ambient=0.28, diffuse=0.8)
pl.camera_position = [(49.75 - 60, 62 - 150, 120), (49.75, 62, -6), (0, 0, 1)]
pl.add_text("As printed: tray on its back, tub on its pod face. No supports", position="upper_left", font_size=12, color="white")
pl.screenshot(str(HERE / "a2_parts.png")); pl.close()
