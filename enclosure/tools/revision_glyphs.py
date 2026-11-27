# /// script
# requires-python = ">=3.10"
# dependencies = ["shapely>=2.0", "matplotlib>=3.8"]
# ///
"""Draws the revision engraving on the back of the tray as ScanRuler sketches.

    uv run enclosure/tools/revision_glyphs.py OUT_DIR [--preview]

ScanRuler's sketches have no text and its parameters are numbers, so the revision is engraved
from the sketches this script writes, and the model picks the digit itself:

- `rev_prefix.json`, the sketch `Rev prefix plan`: "v0." at the back face (z = bottom_z),
  cut straight into the tray like the other back grooves.
- `rev_digits.json`, the sketch `Rev digits plan`: a strip of the ten digits, 0 at the
  engraving's place and each next one `PITCH` further along +X, drawn 0.2 mm below the back
  face and 0.2 mm fatter a side, so that their 45° walls come to the glyph's size at the face.
  The model extrudes the strip as bodies of their own, moves them by -rev × rev_pitch, keeps
  what falls in a window round the place, and cuts the tray with that digit.
- `rev_window.json`, the sketch `Rev window plan`: that window.
- `rev_info.json`: the region points for the two extrudes, and where everything landed.

So a revision bump is the parameter `rev` alone. This script only redraws the glyphs: run it
when the font, the size or the place changes, and send each JSON as the `sketch` of its
feature (`reverse_feature_update { feature, set: { sketch } }`), and `rev_info.json`'s
region points as the `regions` of `Rev prefix` and `Rev digits`. `--preview` also writes
`rev_preview.png`, the back face as it reads (thumbwheel side up) with the engraving, and
`rev_font.png`, every glyph.

The font: strokes 1.0 mm wide at the face (the hatch line's width), 45° chamfers on the
outer corners like the hatch outline and the corner caps, digits 3.6 × 7.0 on a 7-segment
plan, so every stroke end is square and wide enough for the 45° groove walls (0.4 deep)
to leave a flat at the bottom.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

from shapely import affinity
from shapely.geometry import LineString, MultiPolygon, box
from shapely.ops import unary_union

# Glyph cell, mm.
W = 3.6  # digit width
H = 7.0  # digit height (cap height): the hazard band's height, haz_h
XH = 5.0  # x-height of the "v"
S = 1.0  # stroke width at the back face (line_w)
K = 0.8  # chamfer on stroke centre lines; the outer corner's leg is K + S/2·tan(22.5°)
GAP = 1.0  # between glyph cells
DOT = 1.2  # the "." square
DOT_GAP = 0.9

# Stroke centre lines.
L, R = S / 2, W - S / 2
B, T, M = S / 2, H - S / 2, H / 2

# Place on the back (world mm, the back face). The text reads from +X to -X with +Y up, as the
# frog does (the device turned over side to side). Its reading-left edge is the vent slots'
# outer edge (x 90.0: batt_cx + hatch_w/2 + vent_gap + 4 × 2.4 + vent_w), and its baseline
# and digit tops the hazard band's (y 0.1 .. 7.1).
TEXT_LEFT_X = 90.0
BASELINE_Y = 0.1

PITCH = 6.0  # digit strip pitch along +X
GROW = 0.2  # the strip is drawn this far below the face and this much fatter a side
WINDOW_MARGIN = 0.5  # window round the digit cell (half the strip's gap less this)


def chamfered(points: list[tuple[float, float]], closed: bool, corners: set[int] | None = None,
              k: float = K) -> list[tuple[float, float]]:
    """Cuts a 45° chamfer of leg k into the square corners of a centre line (all, or `corners`)."""
    n = len(points)
    out: list[tuple[float, float]] = []
    for i, p in enumerate(points):
        interior = closed or 0 < i < n - 1
        if not interior or (corners is not None and i not in corners):
            out.append(p)
            continue
        a, c = points[i - 1], points[(i + 1) % n]
        da = (a[0] - p[0], a[1] - p[1])
        dc = (c[0] - p[0], c[1] - p[1])
        la, lc = math.hypot(*da), math.hypot(*dc)
        out.append((p[0] + da[0] / la * k, p[1] + da[1] / la * k))
        out.append((p[0] + dc[0] / lc * k, p[1] + dc[1] / lc * k))
    return out


def stroke(points, closed=False, corners=None, k=K):
    pts = chamfered(points, closed, corners, k)
    if closed:
        pts = pts + [pts[0]]
    return LineString(pts).buffer(S / 2, cap_style="flat", join_style="mitre", mitre_limit=5)


def glyph(*strokes, height=H, width=W):
    return unary_union(strokes).intersection(box(0, 0, width, height))


GLYPHS = {
    "0": glyph(stroke([(L, B), (R, B), (R, T), (L, T)], closed=True)),
    "1": glyph(stroke([(0.6, T - 1.5), (2.1, T), (2.1, -1)], corners=set()),
               stroke([(0.5, B), (3.1, B)])),
    "2": glyph(stroke([(-1, T), (R, T), (R, M), (L, M), (L, B), (W + 1, B)])),
    "3": glyph(stroke([(-1, T), (R, T), (R, B), (-1, B)]),
               stroke([(1.0, M), (R, M)])),
    "4": glyph(stroke([(L, H + 1), (L, 2.5), (W + 1, 2.5)]),
               stroke([(R, -1), (R, H + 1)])),
    "5": glyph(stroke([(W + 1, T), (L, T), (L, M), (R, M), (R, B), (-1, B)])),
    "6": glyph(stroke([(W + 1, T), (L, T), (L, B), (R, B), (R, M), (L, M)], corners={1, 2, 3, 4})),
    "7": glyph(stroke([(-1, T), (R, T), (R, -1)])),
    "8": glyph(stroke([(L, B), (R, B), (R, T), (L, T)], closed=True),
               stroke([(L, M), (R, M)])),
    "9": glyph(stroke([(-1, B), (R, B), (R, T), (L, T), (L, M), (R, M)], corners={1, 2, 3, 4})),
    # The v's tip is a clean 90° point on the baseline: its centre line meets at y = S/2·√2.
    "v": glyph(stroke([(L, XH + 1), (L, S / math.sqrt(2) + (W / 2 - L)), (W / 2, S / math.sqrt(2)),
                       (R, S / math.sqrt(2) + (W / 2 - L)), (R, XH + 1)], corners=set()),
               height=XH),
    ".": box(0, 0, DOT, DOT),
}


def layout(text: str) -> list[tuple[str, float]]:
    """Each glyph with its reading-left x in the line."""
    x, out = 0.0, []
    for i, ch in enumerate(text):
        if i:
            x += DOT_GAP if "." in (ch, text[i - 1]) else GAP
        out.append((ch, x))
        x += DOT if ch == "." else W
    return out


def to_world(g, gx: float, x_left: float, y0: float):
    """Glyph space (x right, y up, as read) to the back face: x runs to -X, y to +Y."""
    g = affinity.translate(g, xoff=gx)
    g = affinity.scale(g, xfact=-1, yfact=1, origin=(0, 0))
    return affinity.translate(g, xoff=x_left, yoff=y0)


def rings(geom):
    """Each outline and counter as (ring, is_counter)."""
    polys = geom.geoms if isinstance(geom, MultiPolygon) else [geom]
    for p in polys:
        yield p.exterior, False
        for hole in p.interiors:
            yield hole, True


def region_points(geom) -> list[dict]:
    """A point inside each glyph's strokes, for the extrude's region pick. Not "all": that takes
    the counters inside 0, 6, 8 and 9 as regions too, and cuts them out."""
    polys = geom.geoms if isinstance(geom, MultiPolygon) else [geom]
    return [{"at": [round(c, 4) for c in p.representative_point().coords[0]]} for p in polys]


def sketch_json(geom) -> dict:
    shapes = []
    for ring, _ in rings(geom):
        ring = ring.simplify(1e-6)
        pts = [[round(x, 4), round(y, 4)] for x, y in list(ring.coords)[:-1]]
        shapes.append({"kind": "polyline", "points": pts, "closed": True, "fixed": True})
    return {"shapes": shapes, "infer": False}


def check(geom, name: str, depth: float) -> None:
    """Every edge must outlast the 45° walls: at the groove floor it is shorter by depth·tan(θ/2)
    at each end, θ the turn there (convex positive, concave negative)."""
    worst, where = math.inf, None
    for ring, hole in rings(geom):
        pts = list(ring.coords)[:-1]
        n = len(pts)
        ccw = ring.is_ccw
        turns = []
        for i in range(n):
            a, p, c = pts[i - 1], pts[i], pts[(i + 1) % n]
            cross = (p[0] - a[0]) * (c[1] - p[1]) - (p[1] - a[1]) * (c[0] - p[0])
            ang = math.atan2(cross, (p[0] - a[0]) * (c[0] - p[0]) + (p[1] - a[1]) * (c[1] - p[1]))
            convex = (cross > 0) == ccw
            if hole:
                convex = not convex
            turns.append(abs(ang) if convex else -abs(ang))
        for i in range(n):
            p, c = pts[i], pts[(i + 1) % n]
            length = math.dist(p, c)
            shrink = sum(depth * math.tan(t / 2) for t in (turns[i], turns[(i + 1) % n]))
            if length - shrink < worst:
                worst, where = length - shrink, (p, c)
    print(f"{name}: shortest edge at the groove floor {worst:.3f} mm, {where}", file=sys.stderr)
    if worst <= 0.05:
        raise SystemExit(f"{name}: an edge closes up under the 45° walls")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    placed = layout("v0.0")
    prefix = unary_union([to_world(GLYPHS[ch], gx, TEXT_LEFT_X, BASELINE_Y) for ch, gx in placed[:3]])
    digit_gx = placed[3][1]
    digit_left = TEXT_LEFT_X - digit_gx  # world x of the digit's reading-left (high-x) edge

    strip = unary_union([
        to_world(GLYPHS[str(k)].buffer(GROW, join_style="mitre"), 0, digit_left + k * PITCH, BASELINE_Y)
        for k in range(10)
    ])
    check(prefix, "prefix", 0.4)
    check(strip, "digit strip", 0.4 + GROW)

    (out / "rev_prefix.json").write_text(json.dumps(sketch_json(prefix)))
    (out / "rev_digits.json").write_text(json.dumps(sketch_json(strip)))
    window = (digit_left - W - (PITCH - W) / 2 + WINDOW_MARGIN, BASELINE_Y - 1,
              digit_left + (PITCH - W) / 2 - WINDOW_MARGIN, BASELINE_Y + H + 1)
    (out / "rev_window.json").write_text(json.dumps({"shapes": [
        {"kind": "rectangle", "from": [round(window[0], 4), round(window[1], 4)],
         "to": [round(window[2], 4), round(window[3], 4)], "fixed": True}]}))
    info = {
        "text_box": [round(v, 3) for v in prefix.union(to_world(GLYPHS["0"], digit_gx, TEXT_LEFT_X, BASELINE_Y)).bounds],
        "digit_cell_x": [round(digit_left - W, 3), round(digit_left, 3)],
        "window": [round(v, 3) for v in window],
        "pitch": PITCH,
        "grow": GROW,
        "prefix_regions": region_points(prefix),
        "digit_regions": region_points(strip),
    }
    (out / "rev_info.json").write_text(json.dumps(info, indent=1))
    print(json.dumps(info), file=sys.stderr)

    if "--preview" in sys.argv:
        preview(out, prefix, digit_gx)


def preview(out: Path, prefix, digit_gx: float) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle
    from matplotlib.patches import Polygon as Patch

    fig, axes = plt.subplots(1, 2, figsize=(16, 6), gridspec_kw={"width_ratios": [3, 2]})
    for ax, full in zip(axes, (True, False)):
        ax.set_aspect("equal")
        ax.set_facecolor("#c8692f")
        # Back outline, hatch, hazard stripes, vents, countersinks (from the parameters).
        ax.add_patch(Patch([(-2.4, -2.25), (101.9, -2.25), (101.9, 62.25), (-2.4, 62.25)], fill=False, lw=1))
        ax.add_patch(Patch([(5.5, 9.7), (74.2, 9.7), (74.2, 57.9), (5.5, 57.9)], fill=False, lw=1, ls="--"))
        for c in (25.85, 29.85, 33.85, 37.85, 41.85, 45.85, 49.85, 53.85):
            ax.add_patch(Patch([(c - 4.3, 0.1), (c - 2.7, 0.1), (c + 4.3, 7.1), (c + 2.7, 7.1)], color="#5a2a10"))
        for i in range(5):
            x = 79.2 + 2.4 * i
            ax.add_patch(Patch([(x, 9.7), (x + 1.2, 9.7), (x + 1.2, 57.9), (x, 57.9)], color="#5a2a10"))
        for cx in (2.7, 96.8):
            for cy in (2.7, 57.3):
                ax.add_patch(Circle((cx, cy), 3.0, fill=False, lw=1))
        text = prefix.union(to_world(GLYPHS["4"], digit_gx, TEXT_LEFT_X, BASELINE_Y))
        for poly in (text.geoms if isinstance(text, MultiPolygon) else [text]):
            ax.add_patch(Patch(list(poly.exterior.coords), color="#2b1408"))
            for hole in poly.interiors:
                ax.add_patch(Patch(list(hole.coords), color="#c8692f"))
        if full:
            ax.set_xlim(104, -4.5)
            ax.set_ylim(-4, 64)
        else:
            ax.set_xlim(100, 66)
            ax.set_ylim(-3, 12)
    fig.tight_layout()
    fig.savefig(out / "rev_preview.png", dpi=110)

    # All ten digits, as read.
    fig, ax = plt.subplots(figsize=(12, 2.4))
    ax.set_aspect("equal")
    for k in range(10):
        g = affinity.translate(GLYPHS[str(k)], xoff=k * (W + GAP))
        for poly in (g.geoms if isinstance(g, MultiPolygon) else [g]):
            ax.add_patch(Patch(list(poly.exterior.coords), color="black"))
            for hole in poly.interiors:
                ax.add_patch(Patch(list(hole.coords), color="white"))
    for i, ch in enumerate("v."):
        g = affinity.translate(GLYPHS[ch], xoff=10 * (W + GAP) + i * (W + GAP))
        ax.add_patch(Patch(list(g.exterior.coords), color="black"))
    ax.set_xlim(-0.5, 12 * (W + GAP))
    ax.set_ylim(-0.5, H + 0.5)
    ax.axis("off")
    fig.tight_layout()
    fig.savefig(out / "rev_font.png", dpi=110)


if __name__ == "__main__":
    main()
