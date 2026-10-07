// Intercom enclosure, prototype for 3D printing (OpenSCAD, development snapshot).
//
//   ./render.sh                     # STLs, preview PNGs and a collision check in out/
//   openscad intercom.scad          # interactive; set `part` below or with -D part="shell"
//
// Design coordinates (mm), looking at the front: X right (0..180 = plate width), Y up
// (0..90 = plate height), Z into the wall (0 = front face of the plate). That frame is
// left-handed, the way you'd read the plate drawing and KiCad's top view. Every output
// goes through real(), which mirrors it into the right-handed frame CAD and slicers use.
// Without that mirror, the printed shell would be a mirror image of the real box (and the
// carrier's mounting holes wouldn't line up).
//
// Parts:
// - shell: open at the front; the brass plate sits in a rebate, flush with the front.
//   Inside: plate bosses (M3 heat-set inserts), carrier standoffs (M3 inserts), a closed
//   chamber behind the speaker, bottom vents, a cable opening in the back.
// - wall_plate: screws onto the in-wall box (60 mm screw spacing, horizontal or vertical).
//   The shell hooks onto its top lugs, slides down SLIDE mm, and is fixed with two M3
//   screws from below into the wall plate's tabs. The model shows the final position.
// - mic_tube: optional, glued behind a grille opening; leads sound to the P4's mic.
//
// Every number that a test print may change is a parameter below.

part = "assembly";  // assembly | shell | wall_plate | mic_tube | section
                    // or a collision probe: "hit" with hit_a / hit_b (see render.sh)
hit_a = "shell";
hit_b = "speaker";
SECTION_X = 120;  // where the section preview slices (design X)

$fn = 48;
E = 0.01;  // overlap for clean boolean cuts

// --- plate (from the DXF: 180 x 90 x 3, M3 holes 6 mm from the edges) ---
PLATE_W = 180; PLATE_H = 90; PLATE_T = 3;
PLATE_HOLES = [[6, 6], [174, 6], [6, 84], [174, 84]];
BUTTONS_X = [42, 74, 106, 138]; BUTTON_Y = 14;
BUTTON_HOLE_D = 19.4;
BUTTON_BODY_D = 19; BUTTON_BODY_L = 43;  // body plus plug, behind the plate (measure!)
GRILLE = [8, 25, 172, 82];  // x0, y0, x1, y1 of the grille openings

// --- shell ---
WALL = 2.4;          // side walls, outside the plate edge
BACK = 3.0;          // back wall
REBATE_GAP = 0.3;    // clearance around the plate in its rebate
INNER_DEPTH = 46;    // plate back -> back wall inside; the buttons set this
OUTER_W = PLATE_W + 2 * (WALL + REBATE_GAP);
OUTER_H = PLATE_H + 2 * (WALL + REBATE_GAP);
DEPTH = PLATE_T + INNER_DEPTH + BACK;
Z_BACK = PLATE_T + INNER_DEPTH;  // inner face of the back wall
O = WALL + REBATE_GAP;           // outer edge offset from the plate edge

// Heat-set inserts (M3): hole as the insert maker recommends.
M3_INSERT_D = 4.0; M3_INSERT_L = 5.7;
M3_CLEAR_D = 3.4;
BOSS_D = 8; BOSS_L = 12;  // plate bosses reach this far behind the plate

// --- electronics: carrier board (../carrier) with the Waveshare ESP32-P4-ETH on it ---
// The carrier lies on standoffs against the back wall, components (and the P4) facing the
// plate. Seen from the front it reads like KiCad's top view: RJ45 end left, JSTs at the
// bottom (towards the buttons).
CARRIER_X0 = 86; CARRIER_Y0 = 27;  // box position of the carrier's lower left corner
CARRIER_L = 90; CARRIER_W = 48; PCB_T = 1.6;
STANDOFF_H = 5;  // through-hole leads stick out ~2 mm under the carrier
STANDOFF_D = 7;
// KiCad board coordinates (outline 100..190 x 100..148) -> box X/Y.
function carrier_xy(p) = [CARRIER_X0 + (p[0] - 100), CARRIER_Y0 + (148 - p[1])];
CARRIER_HOLES = [for (p = [[103, 103.5], [187.2, 121], [121.85, 144.3], [166.25, 144.3]]) carrier_xy(p)];
Z_CARRIER = Z_BACK - STANDOFF_H - PCB_T;  // the carrier's component side
SOCKET_H = 8.5;                          // 1x20 female sockets
Z_P4 = Z_CARRIER - SOCKET_H - PCB_T;     // the P4's component side
CARRIER_PARTS_H = 16;  // JST-XH with its plug; C5 is 11.5
// P4 STEP-model coordinates (x across 0..21, y along 0..78, RJ45 at y=0) -> box X/Y.
function p4_xy(mx, my) = [92 + my, 73 - mx];
MIC_XY = p4_xy(4.0, 73.25);  // MIC1, on the P4's component side; top-port (hole in its lid,
                             // checked on the board), so it hears towards the plate
RJ45_H = 13.3;
POE_H = 13;          // PoE module (B) on the 6-pin header: socket ~9 mm, then its PCB and parts
POE_Y = [20, 52];    // along the P4
POE_W = 26;          // slightly wider than the P4
RJ45_PLUG_L = 22;    // plug plus the cable's bend, in front of the jack

// --- speaker: 40 x 40 x 18, in a closed chamber ---
SPK = 40; SPK_H = 18;
SPK_CENTER = [40, 54];
CHAMBER_WALL = 2;

// --- cable and wall plate ---
CABLE_HOLE = [65, 35, 84, 72];  // x0, y0, x1, y1 in the back wall, beside the RJ45 jack
WP_T = 3;
WP_MARGIN = 3;                  // wall plate is this much smaller than the plate
WALLBOX_CENTER = [74, 53];      // in-wall box centre behind the shell (adjust to taste)
WALLBOX_SCREWS = 60;
WALLBOX_SLOT = [10, 4.2];       // slot length, width
SCREW_HEAD = [9, 2.2];          // wall box screw heads: diameter, height
SLIDE = 5;                      // the shell is hung on, then slid down this far
HOOK_X = [100, 160]; HOOK_Y = 80;  // top hooks, above the carrier
TAB_X = [58, 122];              // bottom tabs + screws: in the gaps between the buttons
TAB_Z = Z_BACK - 5;             // height of the fixing screws

// --- vents (bottom face) ---
VENT_L = 30; VENT_W = 2.5; VENT_PITCH = 6;
VENT_X = [86, 176];  // below the carrier

// ---------------------------------------------------------------------------

module box(x, y, z, dx, dy, dz) { translate([x, y, z]) cube([dx, dy, dz]); }

// A hole along +Y (through the bottom wall), centred on x/z.
module y_hole(x, y0, y1, z, d) {
  translate([x, y0, z]) rotate([-90, 0, 0]) cylinder(d = d, h = y1 - y0);
}

module shell_body() {
  difference() {
    box(-O, -O, 0, OUTER_W, OUTER_H, DEPTH);
    // Hollow from the front, leaving the back wall; the plate sits in the opening.
    box(-REBATE_GAP, -REBATE_GAP, -E, PLATE_W + 2 * REBATE_GAP, PLATE_H + 2 * REBATE_GAP, Z_BACK + E);
  }
  // Plate bosses, joined to the corner walls (not to the back wall).
  for (p = PLATE_HOLES) {
    cx = p[0] < PLATE_W / 2 ? -REBATE_GAP : PLATE_W + REBATE_GAP;
    cy = p[1] < PLATE_H / 2 ? -REBATE_GAP : PLATE_H + REBATE_GAP;
    translate([p[0], p[1], PLATE_T]) cylinder(d = BOSS_D, h = BOSS_L);
    box(min(p[0], cx), min(p[1], cy), PLATE_T, abs(cx - p[0]), abs(cy - p[1]), BOSS_L);
  }
  // Carrier standoffs on the back wall.
  for (p = CARRIER_HOLES)
    translate([p[0], p[1], Z_BACK - STANDOFF_H]) cylinder(d = STANDOFF_D, h = STANDOFF_H + E);
  // Speaker chamber: walls from the back wall to the plate; the speaker sits at the front.
  outer = SPK + 2 * CHAMBER_WALL + 1;
  translate([SPK_CENTER[0], SPK_CENTER[1], PLATE_T + 0.5]) difference() {
    translate([-outer / 2, -outer / 2, 0]) cube([outer, outer, INNER_DEPTH - 0.5 + E]);
    translate([-(SPK + 1) / 2, -(SPK + 1) / 2, -E]) cube([SPK + 1, SPK + 1, INNER_DEPTH]);
  }
  // Ledge that holds the speaker against the plate (18 mm speaker behind 0.5 mm of foam).
  translate([SPK_CENTER[0], SPK_CENTER[1], PLATE_T + SPK_H + 0.5]) difference() {
    translate([-(SPK + 1) / 2, -(SPK + 1) / 2, 0]) cube([SPK + 1, SPK + 1, 2]);
    translate([-(SPK - 6) / 2, -(SPK - 6) / 2, -E]) cube([SPK - 6, SPK - 6, 2 + 2 * E]);
  }
}

module shell() {
  difference() {
    shell_body();
    // Insert holes: plate bosses and carrier standoffs.
    for (p = PLATE_HOLES) translate([p[0], p[1], PLATE_T - E]) cylinder(d = M3_INSERT_D, h = M3_INSERT_L + E);
    for (p = CARRIER_HOLES) translate([p[0], p[1], Z_BACK - STANDOFF_H - E]) cylinder(d = M3_INSERT_D, h = M3_INSERT_L + E);
    // Cable opening in the back wall.
    box(CABLE_HOLE[0], CABLE_HOLE[1], Z_BACK - 1, CABLE_HOLE[2] - CABLE_HOLE[0], CABLE_HOLE[3] - CABLE_HOLE[1], BACK + 2);
    // Recesses in the back for the wall box screw heads. The shell is hung SLIDE mm higher
    // and slid down, so each recess also reaches SLIDE mm further down.
    for (s = wallbox_screws()) {
      d = SCREW_HEAD[0] + (WALLBOX_SLOT[0] - WALLBOX_SLOT[1]) + 1;
      box(s[0] - d / 2, s[1] - d / 2 - SLIDE, DEPTH - SCREW_HEAD[1], d, d + SLIDE, SCREW_HEAD[1] + 1);
    }
    // Slots for the wall plate's hooks (top) and tabs (bottom); see wall_plate().
    for (hx = HOOK_X) box(hx - 7, HOOK_Y - SLIDE - 0.3, Z_BACK - 1, 14, 3 + SLIDE + 0.6, BACK + 2);
    for (tx = TAB_X) {
      box(tx - 5.5, 0, Z_BACK - 1, 11, 8 + SLIDE + 0.6, BACK + 2);
      // Fixing screw from below, up through the bottom wall into the tab.
      y_hole(tx, -O - 1, 0.5, TAB_Z, M3_CLEAR_D);
    }
    // Vents in the bottom under the carrier, clear of the fixing screws.
    for (vx = [VENT_X[0] : VENT_PITCH : VENT_X[1] - VENT_W])
      if (min([for (tx = TAB_X) abs(vx + VENT_W / 2 - tx)]) > 5)
        box(vx, -O - 1, Z_BACK - VENT_L - 2, VENT_W, O + 2, VENT_L);
  }
}

function wallbox_screws() = let (c = WALLBOX_CENTER, h = WALLBOX_SCREWS / 2)
  [[c[0] - h, c[1], 0], [c[0] + h, c[1], 0], [c[0], c[1] - h, 90], [c[0], c[1] + h, 90]];

module slot2d(len, w) { hull() for (s = [-1, 1]) translate([s * (len - w) / 2, 0]) circle(d = w); }

module wall_plate() {
  z0 = DEPTH;  // behind the shell
  difference() {
    box(WP_MARGIN, WP_MARGIN, z0, PLATE_W - 2 * WP_MARGIN, PLATE_H - 2 * WP_MARGIN, WP_T);
    box(CABLE_HOLE[0], CABLE_HOLE[1], z0 - 1, CABLE_HOLE[2] - CABLE_HOLE[0], CABLE_HOLE[3] - CABLE_HOLE[1], WP_T + 2);
    // Wall box screws: horizontal and vertical slots (use the pair that fits your box).
    for (s = wallbox_screws())
      translate([s[0], s[1], z0 - 1]) rotate(s[2]) linear_extrude(WP_T + 2) slot2d(WALLBOX_SLOT[0], WALLBOX_SLOT[1]);
  }
  // Top hooks: a stem through the shell's back wall and a lip resting on its inner face.
  for (hx = HOOK_X) {
    box(hx - 6.5, HOOK_Y, Z_BACK - 2.5, 13, 3, z0 - (Z_BACK - 2.5) + E);  // stem
    box(hx - 6.5, HOOK_Y, Z_BACK - 2.5, 13, 8, 2.2);                       // lip, pointing up
  }
  // Bottom tabs reach into the shell; the shell's screws go up into them (M3 inserts).
  for (tx = TAB_X) difference() {
    box(tx - 5, SLIDE + 0.3, Z_BACK - 10, 10, 8, z0 - (Z_BACK - 10) + E);
    y_hole(tx, SLIDE + 0.3 - E, SLIDE + 0.3 + M3_INSERT_L, TAB_Z, M3_INSERT_D);
  }
}

// Tube from the back of the plate to just in front of the mic (stops clear of the USB-C).
// Glue it behind a grille opening; a foam ring closes the last gap to the P4.
module mic_tube() {
  length = (Z_P4 - 3.6) - PLATE_T;
  translate([MIC_XY[0], MIC_XY[1], PLATE_T]) difference() {
    union() {
      cylinder(r = 3, h = length);
      cylinder(r = 5, h = 1.2);  // glue flange
    }
    translate([0, 0, -E]) cylinder(r = 1.5, h = length + 2 * E);
  }
}

// Simplified plate, for checking the assembly (the real one is the DXF).
module plate_dummy() {
  difference() {
    cube([PLATE_W, PLATE_H, PLATE_T]);
    for (x = BUTTONS_X) translate([x, BUTTON_Y, -1]) cylinder(d = BUTTON_HOLE_D, h = PLATE_T + 2);
    box(GRILLE[0], GRILLE[1], -E, GRILLE[2] - GRILLE[0], GRILLE[3] - GRILLE[1], 1);
  }
}

// --- parts that live in the box, as simple blocks --------------------------

module carrier_stack() {
  c = carrier_xy([100, 148]);
  box(c[0], c[1], Z_CARRIER, CARRIER_L, CARRIER_W, PCB_T);                         // carrier
  box(c[0], c[1], Z_CARRIER - CARRIER_PARTS_H, CARRIER_L, 22, CARRIER_PARTS_H);    // JSTs, C5, ...
  s = p4_xy(21, 0);
  box(s[0], s[1], Z_CARRIER - SOCKET_H, 78, 21, SOCKET_H);                         // sockets
  box(s[0], s[1], Z_P4, 78, 21, PCB_T);                                            // P4
  box(s[0], s[1], Z_P4 - 4.1, 70, 21, 4.1);         // top-side parts; the FPC connector is 4.1
  box(s[0] + 70, s[1], Z_P4 - 3.2, 9.4, 21, 3.2);   // USB end: mic, USB-C (3.2)
  r = p4_xy(18.5, -0.9);
  box(r[0], r[1], Z_P4 - RJ45_H, 21.5, 16, RJ45_H);                                // RJ45
  box(r[0] - RJ45_PLUG_L, r[1], Z_P4 - RJ45_H, RJ45_PLUG_L, 16, RJ45_H);           // its plug
  q = p4_xy(10.5 + POE_W / 2, POE_Y[0]);
  box(q[0], q[1], Z_P4 - POE_H, POE_Y[1] - POE_Y[0], POE_W, POE_H);                // PoE module
}

module speaker() {
  box(SPK_CENTER[0] - SPK / 2, SPK_CENTER[1] - SPK / 2, PLATE_T + 0.5, SPK, SPK, SPK_H);
}

module buttons() {
  for (x = BUTTONS_X) translate([x, BUTTON_Y, PLATE_T]) cylinder(d = BUTTON_BODY_D, h = BUTTON_BODY_L);
}

module item(name) {
  if (name == "shell") shell();
  else if (name == "wall_plate") wall_plate();
  else if (name == "mic_tube") mic_tube();
  else if (name == "electronics") carrier_stack();
  else if (name == "speaker") speaker();
  else if (name == "buttons") buttons();
  else if (name == "plate") plate_dummy();
}

// --- output ----------------------------------------------------------------

// Design frame -> real geometry (mirror about the plate's centre line).
module real() { translate([PLATE_W, 0, 0]) mirror([1, 0, 0]) children(); }

module assembly() {
  color("peru") shell();
  color("silver") wall_plate();
  color("goldenrod") plate_dummy();
  color("dimgray") mic_tube();
  color("steelblue", 0.7) carrier_stack();
  color("steelblue", 0.7) speaker();
  color("steelblue", 0.7) buttons();
}

// A 10 mm slice through the carrier, P4 and PoE module, turned so that a camera looking
// straight down shows it from the side: plate on the left, wall on the right, up is up.
if (part == "section") rotate([0, 90, 0]) real() intersection() {
  assembly();
  box(SECTION_X, -20, -20, 10, PLATE_H + 40, DEPTH + 40);
}

real() {
  if (part == "assembly") assembly();
  else if (part == "shell") shell();
  else if (part == "wall_plate") wall_plate();
  else if (part == "mic_tube") mic_tube();
  // Collision probe: render.sh measures the volume of this intersection.
  else if (part == "hit") intersection() { item(hit_a); item(hit_b); }
}
