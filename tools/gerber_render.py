"""
Minimal Gerber (RS-274X) renderer: lines, arcs, flashes, regions -> PNG.

    python tools/gerber_render.py <gerber.zip> out.png [xmin xmax ymin ymax]

Draws the outline, top copper and top silkscreen from an EasyEDA/JLC Gerber
zip, in mm. Written to find the antenna strip (ANTENNA_TUNING.md) without
installing a viewer; it understands what that export uses and nothing more
(no aperture macros, no step-and-repeat).
"""
import math
import re
import sys
import zipfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Polygon, Rectangle


def parse(text):
    fs = (2, 6)
    unit = 1.0  # mm per unit
    apert = {}
    cur_ap = None
    x = y = 0.0
    interp = "G01"
    quadrant = "G75"
    region = False
    poly = []
    shapes = []  # (kind, data)

    def num(s, intd):
        if s is None:
            return None
        neg = s.startswith("-")
        s = s.lstrip("+-")
        v = int(s) / (10 ** fs[1])
        return -v if neg else v

    tokens = re.split(r"[*%\n\r]+", text)
    for t in tokens:
        t = t.strip()
        if not t:
            continue
        if t.startswith("FSLAX"):
            m = re.match(r"FSLAX(\d)(\d)Y(\d)(\d)", t)
            fs = (int(m.group(1)), int(m.group(2)))
            continue
        if t.startswith("MOIN"):
            unit = 25.4
            continue
        if t.startswith("MOMM"):
            unit = 1.0
            continue
        m = re.match(r"ADD(\d+)([A-Z]),?([\d.X]*)", t)
        if m:
            p = [float(v) * unit for v in m.group(3).split("X") if v]
            apert[int(m.group(1))] = (m.group(2), p)
            continue
        if t.startswith("G36"):
            region = True
            poly = []
            continue
        if t.startswith("G37"):
            if len(poly) > 2:
                shapes.append(("poly", poly))
            region = False
            poly = []
            continue
        for g in re.findall(r"G0?[123]", t):
            interp = {"G1": "G01", "G01": "G01", "G2": "G02", "G02": "G02",
                      "G3": "G03", "G03": "G03"}[g]
        m = re.match(r"^(?:G54)?D(\d+)$", t)
        if m and int(m.group(1)) >= 10:
            cur_ap = int(m.group(1))
            continue
        m = re.match(r"^(?:G0?[123])?(?:X([+-]?\d+))?(?:Y([+-]?\d+))?(?:I([+-]?\d+))?(?:J([+-]?\d+))?D0?([123])$", t)
        if not m:
            continue
        nx = num(m.group(1), 0)
        ny = num(m.group(2), 0)
        nx = x if nx is None else nx * unit
        ny = y if ny is None else ny * unit
        i = num(m.group(3), 0)
        j = num(m.group(4), 0)
        d = int(m.group(5))
        if d == 2:
            if region:
                if len(poly) > 2:
                    shapes.append(("poly", poly))
                poly = [(nx, ny)]
        elif d == 1:
            pts = [(x, y), (nx, ny)]
            if interp in ("G02", "G03") and i is not None and j is not None:
                cx, cy = x + i * unit, y + j * unit
                r = math.hypot(x - cx, y - cy)
                a0 = math.atan2(y - cy, x - cx)
                a1 = math.atan2(ny - cy, nx - cx)
                if interp == "G03":
                    while a1 <= a0:
                        a1 += 2 * math.pi
                else:
                    while a1 >= a0:
                        a1 -= 2 * math.pi
                n = 24
                pts = [(cx + r * math.cos(a0 + (a1 - a0) * k / n),
                        cy + r * math.sin(a0 + (a1 - a0) * k / n)) for k in range(n + 1)]
            if region:
                if not poly:
                    poly = [(x, y)]
                poly.extend(pts[1:])
            else:
                w = apert.get(cur_ap, ("C", [0.1]))[1][0] if cur_ap in apert else 0.1
                shapes.append(("line", (pts, w)))
        elif d == 3:
            shapes.append(("flash", (nx, ny, apert.get(cur_ap))))
        x, y = nx, ny
    return shapes


def draw(ax, shapes, color, alpha=1.0):
    for kind, data in shapes:
        if kind == "poly":
            ax.add_patch(Polygon(data, closed=True, color=color, alpha=alpha, lw=0))
        elif kind == "line":
            pts, w = data
            xs, ys = zip(*pts)
            ax.plot(xs, ys, color=color, lw=max(w * 72 / 25.4 * ax.figure.dpi / 72 * 0.0 + 0.6, 0.4),
                    solid_capstyle="round", alpha=alpha)
        elif kind == "flash":
            fx, fy, ap = data
            if not ap:
                continue
            typ, p = ap
            if typ == "C":
                ax.add_patch(Circle((fx, fy), p[0] / 2, color=color, alpha=alpha, lw=0))
            elif typ in ("R", "O") and len(p) >= 2:
                ax.add_patch(Rectangle((fx - p[0] / 2, fy - p[1] / 2), p[0], p[1],
                                       color=color, alpha=alpha, lw=0))


def main(zpath, out, xlim=None, ylim=None):
    z = zipfile.ZipFile(zpath)
    layers = {n: z.read(n).decode("latin-1") for n in z.namelist()}
    fig, ax = plt.subplots(figsize=(14, 14), dpi=110)
    for name, color, alpha in (("Gerber_BoardOutlineLayer.GKO", "black", 1),
                               ("Gerber_TopLayer.GTL", "#c87533", 0.85),
                               ("Gerber_TopSilkscreenLayer.GTO", "#1060ff", 0.9)):
        draw(ax, parse(layers[name]), color, alpha)
    ax.set_aspect("equal")
    ax.autoscale_view()
    if xlim:
        ax.set_xlim(*xlim)
        ax.set_ylim(*ylim)
    ax.grid(True, lw=0.3, alpha=0.4)
    ax.set_title("Top copper (orange), silkscreen (blue), outline -- mm")
    fig.savefig(out, bbox_inches="tight")
    print("saved", out, ax.get_xlim(), ax.get_ylim())


if __name__ == "__main__":
    a = sys.argv
    if len(a) > 3:
        main(a[1], a[2], (float(a[3]), float(a[4])), (float(a[5]), float(a[6])))
    else:
        main(a[1], a[2])
