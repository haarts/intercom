# Intercom hardware

About ten wall boxes, each with a Waveshare ESP32-P4-WIFI6-POE-ETH, a 40 × 40 mm speaker, four
illuminated push buttons (M19, Φ22 bezel, 3–6 V LED ring) and a 180 × 90 × 3 mm brass front
plate. The box sits on the in-wall box the UTP comes out of, with its back against the wall.

| Path | What | Status |
|---|---|---|
| `plate/front-plate-arches.dxf` | Front plate for the laser cutter | Holes corrected (below). Grille pattern still to check with the cutter. |
| `enclosure/enclosure.py` | Parametric CadQuery model of the 3D-printed prototype: shell and wall plate | First version, not printed yet |
| `carrier/` | KiCad project for the button/LED carrier board | Schematic rev A done; ERC clean. PDF and BOM in `carrier/export/`. PCB layout still to do. |

## GPIO map (header P6, numbering as in Waveshare's schematic)

From [Waveshare's schematic](https://files.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH/ESP32-P4-WIFI6-POE-ETH-Schematic.pdf).
Check pin 1 on the board before trusting the numbers.

| Use | GPIO | Header pin |
|---|---|---|
| Button 1 (PTT) | GPIO22 | 11 |
| Button 2 | GPIO20 | 14 |
| Button 3 | GPIO21 | 12 |
| Button 4 | GPIO23 | 8 |
| LED ring 1 | GPIO32 | 25 |
| LED ring 2 | GPIO33 | 30 |
| LED ring 3 | GPIO4 | 17 |
| LED ring 4 | GPIO5 | 15 |
| Spare | GPIO2, GPIO3 | 22, 20 |
| +5 V | | 1, 3 |
| GND | | 5, 10, 13, 19, 26, 29, 33, 40 |

Pins on the header that are **not** free:

| Pins | Why |
|---|---|
| GPIO0, GPIO1 | 32.768 kHz crystal |
| GPIO6 | 0 Ω to the ESP32-C6 (C6_IO2) |
| GPIO54 | C6_CHIP_PU: switching it resets the C6 |
| GPIO7, GPIO8 | I2C to the audio codec |
| GPIO53 | Speaker amplifier enable |
| GPIO24–27 | USB |
| GPIO36–38 | Strapping pins; 37/38 are also the log UART |
| GPIO45–48 | SD-card power domain (usable, but avoided) |

## Carrier board (to design in KiCad)

The carrier is through-hole only, so it can be hand-soldered. It plugs onto the 2×20 header.

**Per button:**
- **Connector:** a JST-XH 4-pin. Pin 1 = LED+ (+5 V), 2 = LED−, 3 = switch, 4 = GND. The power pins are on the outside, and +5 V is not next to GND.
- **Switch input:** 1 kΩ in series to the GPIO, with 100 nF to GND. The pull-up is the ESP32's own; a pressed button reads low.
- **LED ring:** switched low-side by a BC547, with a 1 kΩ base resistor and a 10 kΩ base pull-down that keeps it off during boot. A series resistor footprint (R_LED) is fitted with 0 Ω until a ring's current at 5 V has been measured.

**Also on the board:** 10 µF on 5 V, and a 1×4 header for the spares (GPIO2, GPIO3, GND, GND).

ESPHome: buttons `INPUT_PULLUP`, `inverted: true`; LEDs as `ledc` outputs (dimming, pulsing).

## Front plate

`plate/front-plate-arches.dxf` is a copy of `intercom-grill-arches.dxf` (2026-09-30) with two fixes:

- **Button holes:** Ø16 → **Ø19.4**, for the M19 switches.
- **Mounting holes:** Ø3.0 → **Ø3.3**, for M3.

The countersink note (layer `INFO_CSK`, Ø6) is unchanged.

Still open:
- the grille's minimum slot and web width, to agree with the laser cutter;
- the switches' thread length and nut size;
- each bezel's top edge touches the bottom of the grille (y = 25 mm). Check that this looks right.

## Enclosure prototype (3D print)

```
python enclosure/enclosure.py        # needs CadQuery (Python ≤ 3.13): pip install cadquery
```

This writes `enclosure/out/shell.stl`, `wall_plate.stl`, `assembly.step` (with the plate, board,
speaker and buttons as blocks), and SVG views. It also checks the parts for collisions.

**Shell, 185.4 × 95.4 × 52 mm:**
- **Plate:** sits in a rebate, flush with the front, and is screwed to four corner bosses with M3 heat-set inserts.
- **Inside, from the left:**
  - the speaker in a closed chamber, which improves the low end; a ledge holds it against the plate;
  - the cable opening;
  - the board on four standoffs with M2.5 inserts, components facing the front and the Ethernet jack pointing at the cable.
- **Bottom:** vent slots under the board. Warm air leaves through the grille.

**Wall plate, 174 × 85 × 3 mm:**
- **Mounting:** it screws onto the in-wall box. Slots fit 60 mm screw spacing, horizontal or vertical. Set `WALLBOX_CENTER` to where your wall box ends up behind the shell.
- **Fixing the shell:** hang the shell on the two top hooks and slide it 5 mm down. Two M3 screws from below go into the wall plate's tabs.

**Print settings:** PETG (or ASA near a heat source), 0.2 mm layers, 4 perimeters around the inserts.
Print the shell with its back on the bed; the rebate and bosses need no supports.

**Hardware per box:**
- 4× M3 heat-set inserts + M3×8 countersunk screws for the plate;
- 2× M3 inserts + M3×16 screws to fix the shell to the wall plate;
- 4× M2.5 inserts + M2.5×6 screws for the board;
- 2 screws for the wall box (usually supplied with it).

### To verify with the first print
- **Mic:** it is on the board, about 36 mm behind the plate (`MIC_XY` in the script), and will sound muffled.
  - Try a tube from the mic to the plate, a foam tube or a printed one, sealed at both ends.
  - Check that it ends behind a grille opening, not a web. If needed, drill a small hole.
- **Cable:** the RJ45 plug and the cable's bend between the jack and the cable opening. A slim patch cable helps.
- **Speaker chamber:** check it is sealed, and add foam between the speaker and the plate.
- **Button depth:** 46 mm inside depth assumes a 33 mm body plus its plug. Measure the real buttons.
