"""Intercom enclosure, prototype for 3D printing (CadQuery).

    python enclosure.py            # writes out/*.stl, out/assembly.step and out/*.svg previews

Coordinates (mm), looking at the front: X right (0..180 = plate width), Y up
(0..90 = plate height), Z into the wall (0 = front face of the plate).

Parts:
- shell: open at the front, the brass plate sits in a rebate flush with the front.
  Inside: plate bosses (M3 heat-set inserts), board standoffs (M2.5 inserts), a
  closed chamber behind the speaker, bottom vents, a cable opening in the back.
- wall_plate: screws onto the in-wall box (60 mm screw spacing, horizontal or
  vertical). The shell hooks onto its top lugs and is fixed with two M3 screws
  from below.

Every number that a test print may change is a parameter below.
"""

from pathlib import Path

import cadquery as cq

OUT = Path(__file__).resolve().parent / "out"

# --- plate (from the DXF: 180 x 90 x 3, M3 holes 6 mm from the edges) ---
PLATE_W, PLATE_H, PLATE_T = 180.0, 90.0, 3.0
PLATE_HOLES = [(6, 6), (174, 6), (6, 84), (174, 84)]
BUTTONS_X, BUTTON_Y = (42, 74, 106, 138), 14.0
GRILLE = (8, 25, 172, 82)  # x0, y0, x1, y1 of the grille openings

# --- shell ---
WALL = 2.4  # side walls (outside the plate edge)
BACK = 3.0  # back wall
REBATE_GAP = 0.3  # clearance around the plate in its rebate
INNER_DEPTH = 46.0  # plate back -> back wall inside; buttons (33 mm body + plug) set this
OUTER_W = PLATE_W + 2 * (WALL + REBATE_GAP)
OUTER_H = PLATE_H + 2 * (WALL + REBATE_GAP)
DEPTH = PLATE_T + INNER_DEPTH + BACK
Z_BACK = PLATE_T + INNER_DEPTH  # inner face of the back wall

# Heat-set inserts: M3 (plate, wall plate), M2.5 (board). Hole = insert's recommended hole.
M3_INSERT_D, M3_INSERT_L = 4.0, 5.7
M25_INSERT_D, M25_INSERT_L = 3.5, 5.0
BOSS_D = 8.0
BOSS_L = 12.0  # plate bosses reach this far behind the plate

# --- board: Waveshare ESP32-P4-WIFI6-POE-ETH, 68 x 55, holes 33 x 48 ---
# Placed component side to the front, Ethernet jack facing -X (towards the cable).
BOARD_X0, BOARD_Y0 = 104.0, 25.0  # board corner (jack end, bottom)
BOARD_L, BOARD_W = 68.0, 55.0
BOARD_HOLES = [(BOARD_X0 + 2 + x, BOARD_Y0 + y) for x in (24.3, 57.3) for y in (3.5, 51.5)]
STANDOFF_H = 7.0  # underside parts reach 4.9 mm
MIC_XY = (BOARD_X0 + 2 + 51.8, BOARD_Y0 + 51.7)  # MIC1 from the STEP model

# --- speaker: 40 x 40 x 18, in a closed chamber ---
SPK = 40.0
SPK_CENTER = (40.0, 54.0)
CHAMBER_WALL = 2.0

# --- cable and wall plate ---
CABLE_HOLE = (66, 30, 100, 72)  # x0, y0, x1, y1 in the back wall
WP_T = 3.0
WP_MARGIN = 3.0  # wall plate is this much smaller than the shell's inner outline
WALLBOX_CENTER = (83.0, 51.0)  # in-wall box centre behind the shell (adjust to taste)
WALLBOX_SCREWS = 60.0
SLIDE = 5.0  # the shell is hung on, then slid down this far
HOOK_X = (75.0, 97.0)  # top hooks: between the speaker chamber and the board
HOOK_Y = 80.0
TAB_X = (58.0, 122.0)  # bottom tabs + screws: in the gaps between the buttons

# --- vents (bottom face) ---
VENT_L, VENT_W, VENT_PITCH = 30.0, 2.5, 6.0
VENT_X = (95.0, 170.0)  # under the board


def _box(x, y, z, dx, dy, dz):
    return cq.Workplane("XY").box(dx, dy, dz, centered=False).translate((x, y, z))


def shell():
    s = cq.Workplane("XY").box(OUTER_W, OUTER_H, DEPTH, centered=False).translate((-WALL - REBATE_GAP, -WALL - REBATE_GAP, 0))
    # Hollow it from the front, leaving the back wall.
    cavity = cq.Workplane("XY").box(PLATE_W + 2 * REBATE_GAP, PLATE_H + 2 * REBATE_GAP, Z_BACK, centered=False).translate((-REBATE_GAP, -REBATE_GAP, 0))
    s = s.cut(cavity)

    # Plate bosses: short, joined to the corner walls (not the back wall), with M3 insert holes.
    for x, y in PLATE_HOLES:
        boss = cq.Workplane("XY").circle(BOSS_D / 2).extrude(BOSS_L).translate((x, y, PLATE_T))
        cx = -REBATE_GAP if x < PLATE_W / 2 else PLATE_W + REBATE_GAP
        cy = -REBATE_GAP if y < PLATE_H / 2 else PLATE_H + REBATE_GAP
        web = _box(min(x, cx), min(y, cy), PLATE_T, abs(cx - x), abs(cy - y), BOSS_L)
        s = s.union(boss).union(web)
        s = s.cut(cq.Workplane("XY").circle(M3_INSERT_D / 2).extrude(M3_INSERT_L).translate((x, y, PLATE_T)))

    # Board standoffs on the back wall.
    for x, y in BOARD_HOLES:
        so = cq.Workplane("XY").circle(3.5).extrude(STANDOFF_H).translate((x, y, Z_BACK - STANDOFF_H))
        s = s.union(so)
        s = s.cut(cq.Workplane("XY").circle(M25_INSERT_D / 2).extrude(M25_INSERT_L).translate((x, y, Z_BACK - STANDOFF_H)))

    # Speaker chamber: walls from the back wall to the plate; the speaker sits at the front.
    cx, cy = SPK_CENTER
    outer = SPK + 2 * CHAMBER_WALL + 1.0
    chamber = (
        cq.Workplane("XY").rect(outer, outer).extrude(INNER_DEPTH - 0.5)
        .cut(cq.Workplane("XY").rect(SPK + 1.0, SPK + 1.0).extrude(INNER_DEPTH))
        .translate((cx, cy, PLATE_T + 0.5))
    )
    s = s.union(chamber)
    # Ledge that stops the speaker 18 mm behind the plate (it is pressed against the plate by it).
    ledge = (
        cq.Workplane("XY").rect(SPK + 1.0, SPK + 1.0).extrude(2.0)
        .cut(cq.Workplane("XY").rect(SPK - 6, SPK - 6).extrude(2.0))
        .translate((cx, cy, PLATE_T + 18.5))
    )
    s = s.union(ledge)

    # Cable opening in the back wall.
    x0, y0, x1, y1 = CABLE_HOLE
    s = s.cut(cq.Workplane("XY").box(x1 - x0, y1 - y0, BACK + 2, centered=False).translate((x0, y0, Z_BACK - 1)))

    # Slots for the wall plate's hooks (top) and tabs (bottom); see wall_plate().
    for hx in HOOK_X:
        s = s.cut(_box(hx - 7, HOOK_Y - SLIDE - 0.3, Z_BACK - 1, 14, 3 + SLIDE + 0.6, BACK + 2))
    for tx in TAB_X:
        s = s.cut(_box(tx - 5.5, 0.0, Z_BACK - 1, 11, 8 + SLIDE + 0.6, BACK + 2))
        # Screw hole up through the bottom wall into the tab.
        s = s.cut(cq.Workplane("XZ").center(tx, -(Z_BACK - 5)).circle(1.7).extrude(-(WALL + REBATE_GAP + 2)).translate((0, -WALL - REBATE_GAP - 1, 0)))
    # Vents in the bottom under the board (skipping the screws).
    vx = VENT_X[0]
    while vx + VENT_W <= VENT_X[1]:
        if all(abs(vx + VENT_W / 2 - tx) > 5 for tx in TAB_X):
            s = s.cut(_box(vx, -WALL - REBATE_GAP - 1, Z_BACK - VENT_L - 2, VENT_W, WALL + 2, VENT_L))
        vx += VENT_PITCH
    return s


def wall_plate():
    """Sits behind the shell; screws to the wall box."""
    w = PLATE_W - 2 * WP_MARGIN
    h = PLATE_H - 2 * WP_MARGIN
    z0 = DEPTH  # behind the shell
    p = cq.Workplane("XY").box(w, h, WP_T, centered=False).translate((WP_MARGIN, WP_MARGIN, z0))
    # Cable opening, same as the shell's.
    x0, y0, x1, y1 = CABLE_HOLE
    p = p.cut(cq.Workplane("XY").box(x1 - x0, y1 - y0, WP_T + 2, centered=False).translate((x0, y0, z0 - 1)))
    # Wall box screws: 60 mm apart, horizontal and vertical slots (pick the pair that fits your box).
    cx, cy = WALLBOX_CENTER
    half = WALLBOX_SCREWS / 2
    for (sx, sy, horizontal) in ((cx - half, cy, True), (cx + half, cy, True), (cx, cy - half, False), (cx, cy + half, False)):
        slot = cq.Workplane("XY").slot2D(10, 4.2, angle=0 if horizontal else 90).extrude(WP_T + 2).translate((sx, sy, z0 - 1))
        p = p.cut(slot)
    # Top hooks: a stem through the shell's back wall and a lip that rests on its inner face.
    for hx in HOOK_X:
        p = p.union(_box(hx - 6.5, HOOK_Y, Z_BACK - 2.5, 13, 3, z0 - (Z_BACK - 2.5)))  # stem
        p = p.union(_box(hx - 6.5, HOOK_Y, Z_BACK - 2.5, 13, 8, 2.2))  # lip, pointing up
    # Bottom tabs reach into the shell; the shell's screws go up into them (M3 inserts).
    for tx in TAB_X:
        tab = _box(tx - 5, SLIDE + 0.3, Z_BACK - 10, 10, 8, z0 - (Z_BACK - 10))
        tab = tab.cut(cq.Workplane("XZ").center(tx, -(Z_BACK - 5)).circle(M3_INSERT_D / 2).extrude(-M3_INSERT_L).translate((0, SLIDE + 0.3, 0)))
        p = p.union(tab)
    return p


def plate_dummy():
    """Simplified plate, for checking the assembly (the real one is the DXF)."""
    p = cq.Workplane("XY").box(PLATE_W, PLATE_H, PLATE_T, centered=False)
    for x in BUTTONS_X:
        p = p.cut(cq.Workplane("XY").circle(19.4 / 2).extrude(PLATE_T + 2).translate((x, BUTTON_Y, -1)))
    gx0, gy0, gx1, gy1 = GRILLE
    p = p.cut(cq.Workplane("XY").box(gx1 - gx0, gy1 - gy0, 1.0, centered=False).translate((gx0, gy0, -0.01)))
    return p


def keep_outs():
    """Parts that live in the box, as simple blocks, to check for collisions."""
    items = {}
    zpcb = Z_BACK - STANDOFF_H  # PCB underside
    items["board"] = cq.Workplane("XY").box(BOARD_L + 2, BOARD_W, 1.6, centered=False).translate((BOARD_X0, BOARD_Y0, zpcb - 1.6))
    items["board_parts"] = cq.Workplane("XY").box(BOARD_L + 2, BOARD_W, 16, centered=False).translate((BOARD_X0, BOARD_Y0, zpcb - 1.6 - 16))
    # Carrier on the header: header at the far end of the board, ~30 mm tall stack incl. plugs.
    items["carrier"] = cq.Workplane("XY").box(14, 55, 30, centered=False).translate((BOARD_X0 + 58, BOARD_Y0, zpcb - 1.6 - 30))
    items["speaker"] = cq.Workplane("XY").box(SPK, SPK, 18, centered=False).translate((SPK_CENTER[0] - SPK / 2, SPK_CENTER[1] - SPK / 2, PLATE_T + 0.5))
    for i, x in enumerate(BUTTONS_X):
        items[f"button{i + 1}"] = cq.Workplane("XY").circle(9.5).extrude(43).translate((x, BUTTON_Y, PLATE_T))
    return items


def check_collisions(sh):
    items = keep_outs()
    problems = []
    names = list(items)
    for i, a in enumerate(names):
        if items[a].intersect(sh).val().Volume() > 1.0:
            problems.append(f"{a} hits the shell")
        for b in names[i + 1:]:
            if {a, b} <= {"board", "board_parts", "carrier"}:
                continue
            if items[a].intersect(items[b]).val().Volume() > 1.0:
                problems.append(f"{a} hits {b}")
    return problems


def main():
    OUT.mkdir(exist_ok=True)
    sh, wp = shell(), wall_plate()
    cq.exporters.export(sh, str(OUT / "shell.stl"), tolerance=0.05, angularTolerance=0.1)
    cq.exporters.export(wp, str(OUT / "wall_plate.stl"), tolerance=0.05, angularTolerance=0.1)
    asm = cq.Assembly()
    asm.add(sh, name="shell", color=cq.Color(0.55, 0.4, 0.25))
    asm.add(wp, name="wall_plate", color=cq.Color(0.6, 0.6, 0.6))
    asm.add(plate_dummy(), name="plate", color=cq.Color(0.8, 0.65, 0.2))
    for name, part in keep_outs().items():
        asm.add(part, name=name, color=cq.Color(0.2, 0.5, 0.8, 0.6))
    asm.save(str(OUT / "assembly.step"))
    # Line-drawing previews: front (through the plate), back, and a section from the side.
    opts = {"showAxes": False, "showHidden": False, "width": 900, "height": 500, "marginLeft": 20, "marginTop": 20}
    cq.exporters.export(sh, str(OUT / "shell_back.svg"), opt={**opts, "projectionDir": (0, 0, 1)})
    cq.exporters.export(sh, str(OUT / "shell_front.svg"), opt={**opts, "projectionDir": (0, 0, -1)})
    cq.exporters.export(sh, str(OUT / "shell_iso.svg"), opt={**opts, "projectionDir": (-1, -1, -1.2)})
    cq.exporters.export(wp, str(OUT / "wall_plate_back.svg"), opt={**opts, "projectionDir": (0, 0, 1)})
    problems = check_collisions(sh)
    bb = sh.val().BoundingBox()
    print(f"shell {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm; wall plate {wp.val().BoundingBox().xlen:.1f} x {wp.val().BoundingBox().ylen:.1f} mm")
    print("collisions:", problems or "none")


if __name__ == "__main__":
    main()
