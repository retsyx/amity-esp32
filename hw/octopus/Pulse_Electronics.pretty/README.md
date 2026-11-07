# E5J88-00LJG2-L footprint

Source: Pulse drawing CE5J88-00LJGX-L, revision 0, dated 2013-03-20
([datasheet](https://www.mouser.com/datasheet/2/447/E5J88_00LJG2_L-2903466.pdf)).

- Component-side view; pin 1 is the origin and the cable opening faces negative Y.
  This rotates the drawing's PCB layout by 180 degrees without mirroring it.
  The front elevation labels contact 8 on the left and contact 1 on the right;
  with the opening down, the rightmost lower PCB hole is therefore pin 1.
- Signal holes: diameter 0.90 mm, 1.02 mm adjacent-column pitch, 1.78 mm row spacing.
  The stated pitch gives a 7.14 mm span, 0.03 mm above the drawing's rounded
  7.11 mm overall dimension and within its +/-0.05 mm PCB-layout tolerance.
- Locating holes: diameter 3.20 mm, non-plated, 12.70 mm apart;
  their row is 2.54 mm toward the opening from pin 1.
- Body: 15.80 x 15.00 mm; its front is 5.10 mm ahead of the locating-hole row.
  PCB hole positions take precedence over the rounded 9.4 mm side-view dimension.
- Copper pads are 1.50 mm diameter (0.30 mm nominal annular ring); pin 1 is rounded square.
  The courtyard clears the body and locating holes by at least 0.50 mm.

Both projects contain an identical copy under the `Pulse_Electronics` library.

## 3D model

The footprint loads `../models/RJ45_Pulse_E5J88-00LJG2-L_Horizontal.step` through
`${KIPRJMOD}` with zero offset/rotation and unit scale. The STEP uses millimeters,
pin 1 as its XY origin, Z=0 at the PCB top, and Y opposite the footprint's Y.
The body bottom is Z=0.50 mm, top Z=13.10 mm, and signal tips Z=-2.67 mm.

This is a reconstruction from the drawing, not a manufacturer-supplied model.
The socket cavity, contact bends, rear feet and retention-clip contours are
visual approximations; the dimensioned body envelope and pin locations are preserved.
Regenerate both copies with `python hw/generate_e5j88_model.py` from the repository
root, using CadQuery 2.8.

The Octopus PCB uses the `_EdgeMount` variant: it omits the cable-facing silk
line and sets the adjacent side lines 0.25 mm inside the PCB edge with the
housing overhanging by 2 mm. Pads, body outline, courtyard and 3D model are unchanged.


## Dual-port E5J88-00AJG2-L

The Octopus uses the two-port part with the `_EdgeMount` footprint and its
matching STEP model. Source: Pulse CE5J88-00AJGX-L Rev 0, dated 2012-09-24
([datasheet](https://www.mouser.com/datasheet/2/447/E5J88_00AJG2_L-2902971.pdf)).
The 31.60 x 15.00 mm housing overhangs the board by 2 mm; port pitch is 15.80 mm.
The body has a 0.60 mm standoff and reaches 13.20 mm above the PCB.

The drawing repeats contact numbers 1-8 for each port. With openings toward
negative footprint Y, the left bank uses pads 1-8 (symbol unit A); the right
bank uses pads 9-16 (unit B), corresponding to local contacts 1-8.
The port ordering, viewed from above with the MCU carrier at the left, is:

| PCB edge | Left port | Right port | Package |
| --- | --- | --- | --- |
| Lower | 1 (unit B) | 2 (unit A) | J1 |
| Upper | 3 (unit A) | 4 (unit B) | J3 |

Both banks use 1.02 mm contact pitch, 1.78 mm row spacing and 0.90 mm signal
drills. There are only two 3.20 mm locating holes, 28.50 mm apart.
Regenerate the model with `python hw/generate_e5j88_dual_model.py` using
CadQuery 2.8. Undimensioned socket and clip details are visual approximations.
