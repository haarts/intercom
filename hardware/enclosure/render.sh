#!/usr/bin/env bash
# Render the enclosure: STLs, preview PNGs and a collision check, into out/.
#
#   ./render.sh                         # uses openscad-nightly if found, else openscad (2025+ snapshot)
#   OPENSCAD=~/bin/OpenSCAD.AppImage ./render.sh
set -euo pipefail
cd "$(dirname "$0")"
SCAD=${OPENSCAD:-$(command -v openscad-nightly || echo openscad)}
OPTS=(--backend=manifold)
mkdir -p out

for part in shell wall_plate mic_tube; do
  "$SCAD" "${OPTS[@]}" -q -D "part=\"$part\"" -o "out/$part.stl" intercom.scad
  echo "out/$part.stl"
done

# Previews. Camera: translate x,y,z, rotate x,y,z, distance (real frame: the front faces -Z).
view() {  # name part camera
  "$SCAD" "${OPTS[@]}" -q -D "part=\"$2\"" --camera="$3" --imgsize=1400,900 --colorscheme=Tomorrow \
    --render -o "out/$1.png" intercom.scad
  echo "out/$1.png"
}
view front assembly 90,45,25,180,0,180,420          # from the room
view back assembly 90,45,25,0,0,180,420             # from the wall
view iso assembly 90,45,25,235,0,325,460            # from the room, above right
view inside shell 90,45,25,180,0,180,420            # the empty shell through its open front
view inside_iso shell 90,45,25,225,0,215,460        # the same, from above left
view section section 28,45,-55,0,0,0,260           # a slice through the carrier, from the side

# Collision check: each pair must not overlap (empty intersection).
pairs=(
  "shell electronics" "shell speaker" "shell buttons" "shell mic_tube" "shell wall_plate"
  "electronics speaker" "electronics buttons" "electronics mic_tube" "electronics wall_plate"
  "speaker buttons" "speaker mic_tube" "buttons mic_tube" "plate shell" "plate electronics"
)
# Volume shared by two items (divergence theorem over the triangles of their intersection).
# Faces that only touch (the carrier on its standoffs, the plate on its bosses) give zero.
overlap() {
  "$SCAD" "${OPTS[@]}" -q -D 'part="hit"' -D "hit_a=\"$1\"" -D "hit_b=\"$2\"" --export-format asciistl \
    -o out/.hit.stl intercom.scad 2>/dev/null || true
  awk '/vertex/ { v[n % 3] = $2 " " $3 " " $4; n++
                  if (n % 3 == 0) { split(v[0], a); split(v[1], b); split(v[2], c)
                    s += (a[1]*(b[2]*c[3]-b[3]*c[2]) - a[2]*(b[1]*c[3]-b[3]*c[1]) + a[3]*(b[1]*c[2]-b[2]*c[1])) / 6 } }
       END { printf "%.2f", (s < 0 ? -s : s) }' out/.hit.stl 2>/dev/null || echo 0
  rm -f out/.hit.stl
}

# The mic tube should end behind a grille opening: how much of its bore does the brass cover?
bore=$(awk 'BEGIN { printf "%.2f", 3.14159265 * 1.5 * 1.5 * 3 }')  # bore area x plate thickness
covered=$(overlap plate mic_bore)
echo "mic tube bore covered by the plate: $(awk -v c="$covered" -v b="$bore" 'BEGIN { printf "%.0f%%", 100 * c / b }')"

problems=0
for p in "${pairs[@]}"; do
  set -- $p
  vol=$(overlap "$1" "$2")
  if awk -v v="$vol" 'BEGIN { exit !(v > 0.5) }'; then
    echo "COLLISION: $1 and $2 overlap by $vol mm3"
    problems=$((problems + 1))
  fi
done
echo "collisions: $problems"
[ "$problems" -eq 0 ]
