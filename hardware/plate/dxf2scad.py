"""Convert the plate's DXF (R12: POLYLINE with bulges, CIRCLE) into OpenSCAD for the enclosure model.

    python3 dxf2scad.py front-plate-arches.dxf > front-plate-arches.scad

OpenSCAD's own DXF import skips R12 POLYLINEs, so the enclosure model includes this file
instead. The DXF stays the source for the laser cutter; rerun this after changing it.
Only the CUT layer is converted: the largest closed shape is the outline, all others are
holes. Standard library only.
"""

import math
import sys

ARC_STEP = math.radians(6)  # arc resolution


def read_pairs(path):
    lines = [line.strip() for line in open(path, encoding="latin-1")]
    return list(zip(lines[0::2], lines[1::2]))


def entities(pairs):
    """Yield (type, {code: [values]}) for each entity in the ENTITIES section."""
    inside, current = False, None
    for code, value in pairs:
        if code == "2" and value == "ENTITIES":
            inside = True
            continue
        if not inside:
            continue
        if code == "0":
            if current:
                yield current
            if value == "ENDSEC":
                return
            current = (value, {})
        elif current:
            current[1].setdefault(code, []).append(value)


def bulge_points(p1, p2, bulge):
    """Points along the arc from p1 to p2 (excluding p1), for a DXF bulge."""
    if abs(bulge) < 1e-9:
        return [p2]
    theta = 4 * math.atan(bulge)  # included angle, signed
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    chord = math.hypot(dx, dy)
    r = chord / (2 * math.sin(theta / 2))
    # Centre: from the chord midpoint, perpendicular, at distance r*cos(theta/2).
    mx, my = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2
    h = r * math.cos(theta / 2)
    cx, cy = mx - h * dy / chord, my + h * dx / chord
    a1 = math.atan2(p1[1] - cy, p1[0] - cx)
    n = max(2, math.ceil(abs(theta) / ARC_STEP))
    return [(cx + abs(r) * math.cos(a1 + theta * k / n), cy + abs(r) * math.sin(a1 + theta * k / n)) for k in range(1, n + 1)]


def shapes(path, layer="CUT"):
    out = []
    poly, verts = None, []
    for kind, data in entities(read_pairs(path)):
        on_layer = data.get("8", [""])[0] == layer
        if kind == "POLYLINE":
            poly, verts = (on_layer, int(data.get("70", ["0"])[0]) & 1), []
        elif kind == "VERTEX" and poly is not None:
            verts.append((float(data["10"][0]), float(data["20"][0]), float(data.get("42", ["0"])[0])))
        elif kind == "SEQEND" and poly is not None:
            keep, closed = poly
            if keep and verts:
                pts = [verts[0][:2]]
                segs = list(zip(verts, verts[1:] + ([verts[0]] if closed else [])))
                for (x1, y1, b), (x2, y2, _) in segs:
                    pts += bulge_points((x1, y1), (x2, y2), b)
                if closed and pts[-1] == pts[0]:
                    pts.pop()
                out.append(pts)
            poly = None
        elif kind == "CIRCLE" and on_layer:
            cx, cy, r = (float(data[c][0]) for c in ("10", "20", "40"))
            n = max(24, math.ceil(2 * math.pi / ARC_STEP))
            out.append([(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n)) for k in range(n)])
    return out


def area(pts):
    return abs(sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]))) / 2


def main(path):
    found = shapes(path)
    outline = max(found, key=area)
    holes = [s for s in found if s is not outline]

    def poly(pts):
        return "polygon([" + ",".join(f"[{x:.3f},{y:.3f}]" for x, y in pts) + "]);"

    print(f"// Generated from {path.split('/')[-1]} by dxf2scad.py; do not edit.")
    print(f"// {len(holes)} holes. The DXF is the source for the laser cutter.")
    print("module plate_2d() {")
    print("  difference() {")
    print("    " + poly(outline))
    for h in holes:
        print("    " + poly(h))
    print("  }")
    print("}")


if __name__ == "__main__":
    main(sys.argv[1])
