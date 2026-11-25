# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["pyvista>=0.44", "numpy", "pillow"]
# ///
"""v0.5, A and B side by side: a back view, a side and an end elevation, put together as
concepts_sheet.png. Run build_pod.py and build_bevel.py first.

    uv run enclosure/concepts/flatter-back/render.py
"""
from pathlib import Path
import pyvista as pv
from PIL import Image

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
pv.OFF_SCREEN = True
TRAY, BEZEL, BG = "#b5561f", "#3b4049", "#2d3035"
C = (49.75, 30.0, -3.2)
SETS = [(BUILD / "v05_tray.stl", BUILD / "v05_bezel.stl", "v0.5 — flat back, 11.86"),
        (HERE / "podA_tray.stl", HERE / "podA_bezel.stl", "A · pod back — 11.86 at the pod, 9.95 elsewhere"),
        (HERE / "bevelB_tray.stl", BUILD / "v05_bezel.stl", "B · bevelled edge — 11.86, edges read 9.86")]

def shot(out, cam, size, parallel=None, rows=False):
    shape = (len(SETS), 1) if rows else (1, len(SETS))
    pl = pv.Plotter(shape=shape, window_size=size, off_screen=True, border=False)
    for i, (t, b, title) in enumerate(SETS):
        pl.subplot(*((i, 0) if rows else (0, i))); pl.set_background(BG)
        pl.add_mesh(pv.read(t), color=TRAY, specular=0.15, ambient=0.28, diffuse=0.8)
        pl.add_mesh(pv.read(b), color=BEZEL, specular=0.1, ambient=0.28, diffuse=0.8)
        pl.camera_position = cam
        if parallel:
            pl.enable_parallel_projection(); pl.camera.parallel_scale = parallel
        pl.add_text(title, position="upper_left", font_size=11 if rows else 12, color="white")
    pl.screenshot(str(out)); pl.close()

shot(BUILD / "cmp_back_iso.png", [(C[0]+95, C[1]-150, C[2]-120), C, (0, 0, 1)], (2400, 760))
shot(BUILD / "cmp_side.png", [(C[0], C[1]-300, C[2]), C, (0, 0, 1)], (1500, 780), parallel=10.6, rows=True)
shot(BUILD / "cmp_end.png", [(C[0]+300, C[1], C[2]), C, (0, 0, 1)], (1100, 780), parallel=10.6, rows=True)

# the sheet: back view on top, the side (USB-C end on the left) and the antenna end below it
a = Image.open(BUILD / "cmp_back_iso.png").crop((0, 0, 2400, 590))
b = Image.open(BUILD / "cmp_side.png"); c = Image.open(BUILD / "cmp_end.png")
bw = 1400; b = b.resize((bw, int(b.height * bw / b.width)))
cw = 2400 - bw; c = c.resize((cw, int(c.height * cw / c.width)))
row2 = Image.new("RGB", (2400, max(b.height, c.height)), BG); row2.paste(b, (0, 0)); row2.paste(c, (bw, 0))
sheet = Image.new("RGB", (2400, a.height + row2.height), BG); sheet.paste(a, (0, 0)); sheet.paste(row2, (0, a.height))
sheet.save(HERE / "concepts_sheet.png"); print(sheet.size)
