#!/usr/bin/env python3
"""Rebuild the E5J88-00AJG2-L dual-port STEP model using CadQuery 2.8.

Run from the repository root: python hw/generate_e5j88_dual_model.py
Use --output /tmp/connector.step to export without changing project assets.
Source: Pulse CE5J88-00AJGX-L Rev 0 (2012-09-24), linked in the footprint README.
Dimensions are millimeters; Z=0 is the PCB top. KiCad model Y is opposite
footprint Y, so both cable openings face positive model Y. The model has one
shared housing. Undimensioned socket details and retention clips are visual
approximations. The left bank is numbered 1-8, the right bank 9-16.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import cadquery as cq


MODEL_NAME = "RJ45_Pulse_E5J88-00AJG2-L_Horizontal"
BODY_WIDTH, BODY_DEPTH, BODY_HEIGHT = 31.60, 15.00, 12.60
PCB_HEIGHT = 13.20
STANDOFF = PCB_HEIGHT - BODY_HEIGHT
PORT_PITCH = 15.80
PIN_PITCH, ROW_SPACING, PIN_THICKNESS, PIN_PROTRUSION = 1.02, 1.78, 0.35, 2.67
PEG_SPACING, PEG_FRONT_OFFSET = 28.50, 5.10
LEFT_PORT_X = 3.5 * PIN_PITCH
CENTER_X = LEFT_PORT_X + PORT_PITCH / 2
PEG_Y = 4.32 - ROW_SPACING
FRONT_Y = PEG_Y + PEG_FRONT_OFFSET
REAR_Y = FRONT_Y - BODY_DEPTH


def block(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY")
            .box(x1-x0, y1-y0, z1-z0, centered=False)
            .translate((x0, y0, z0)).val())


def terminal_centers():
    """Return the two banks in the footprint's global pin-number order."""
    for port in range(2):
        for index in range(8):
            yield (port * 8 + index + 1,
                   port * PORT_PITCH + index * PIN_PITCH,
                   -ROW_SPACING if index % 2 else 0.0)


def contact(x, pin_y):
    """Approximate the internal stamped contact with the drawn 0.35 mm stock."""
    half = PIN_THICKNESS / 2
    bridge = block(x-half, x+half, -3.125, pin_y+half, 1.25, 1.60)
    bend = block(x-half, x+half, -3.125, -2.775, 1.25, 5.30)
    spring = (cq.Workplane("YZ", origin=(x-half, 0, 0))
              .polyline([(-3.125, 4.95), (6.50, 2.70),
                         (6.50, 3.05), (-3.125, 5.30)])
              .close().extrude(PIN_THICKNESS).val())
    return bridge.fuse(bend, spring).clean()


def build_model():
    x0, x1 = CENTER_X-BODY_WIDTH/2, CENTER_X+BODY_WIDTH/2
    body = block(x0, x1, REAR_Y, FRONT_Y, STANDOFF, PCB_HEIGHT)

    # Two independent mouths in a single shared housing. Their depth and
    # internal stepped contours are visual approximations of the elevation.
    mouth = [(-6.50, 2.30), (6.50, 2.30), (6.50, 9.90), (3.85, 9.90),
             (3.85, 11.50), (2.05, 11.50), (2.05, 12.80),
             (-2.05, 12.80), (-2.05, 11.50), (-3.85, 11.50),
             (-3.85, 9.90), (-6.50, 9.90)]
    for port_x in (LEFT_PORT_X, LEFT_PORT_X+PORT_PITCH):
        mouth_plane = cq.Plane(origin=(port_x, FRONT_Y+0.10, 0),
                               xDir=(1, 0, 0), normal=(0, -1, 0))
        cavity = (cq.Workplane(mouth_plane).polyline(mouth).close()
                  .extrude(10.90).val())
        body = body.cut(cavity)

    # Approximate rear feet establish the derived 0.60 mm body standoff.
    for a, b in ((x0, x0+2.0), (x1-2.0, x1)):
        body = body.fuse(block(a, b, REAR_Y, REAR_Y+4.0, 0, STANDOFF+0.1))

    # The source locates two outer clips, not one pair beneath each port.
    # Clip contours are approximate and fit inside the specified 3.20 mm holes.
    for x in (CENTER_X-PEG_SPACING/2, CENTER_X+PEG_SPACING/2):
        peg = (cq.Workplane("XY").center(x, PEG_Y).circle(1.50)
               .extrude(3.50+STANDOFF+0.15).translate((0, 0, -3.50))
               .edges("<Z").chamfer(0.35).val())
        slit = block(x-0.30, x+0.30, PEG_Y-2.0, PEG_Y+2.0, -3.60, -0.20)
        body = body.fuse(peg.cut(slit))

    terminals, contacts = [], []
    half = PIN_THICKNESS / 2
    centers = list(terminal_centers())
    for _, x, y in centers:
        terminals.append(block(x-half, x+half, y-half, y+half,
                               -PIN_PROTRUSION, 1.60))
        contacts.append(contact(x, y))

    # Assign embedded metal to its material rather than overlapping the nylon.
    metal = cq.Compound.makeCompound(terminals + contacts)
    body = body.cut(metal).clean()
    assert body.isValid() and len(body.Solids()) == 1
    bounds = body.BoundingBox()
    for actual, expected in ((bounds.xlen, BODY_WIDTH), (bounds.ylen, BODY_DEPTH),
                             (bounds.zmax, PCB_HEIGHT)):
        assert abs(actual-expected) < 1e-6, (actual, expected)

    assembly = cq.Assembly(name=MODEL_NAME)
    assembly.add(body, name="shared_black_nylon_housing",
                 color=cq.Color(0.10, 0.105, 0.115))
    for (number, x, y), terminal, spring in zip(centers, terminals, contacts):
        gold_contact = spring.cut(terminal).clean()
        assert terminal.isValid() and gold_contact.isValid()
        bounds = terminal.BoundingBox()
        assert abs(bounds.zmin + PIN_PROTRUSION) < 1e-6
        assert abs((bounds.xmin+bounds.xmax)/2-x) < 1e-6
        assert abs((bounds.ymin+bounds.ymax)/2-y) < 1e-6
        assert (bounds.xlen**2+bounds.ylen**2)**0.5 < 0.90
        assembly.add(terminal, name=f"pin_{number}_tin_tail",
                     color=cq.Color(0.72, 0.74, 0.77))
        assembly.add(gold_contact, name=f"pin_{number}_gold_contact",
                     color=cq.Color(0.83, 0.64, 0.20))
    return assembly


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Export one STEP to this path")
    args = parser.parse_args()
    hardware = Path(__file__).resolve().parent
    output = args.output or hardware / "octopus" / "models" / (MODEL_NAME + ".step")
    output.parent.mkdir(parents=True, exist_ok=True)
    build_model().export(str(output), exportType="STEP", unit="MM")
    shape = cq.importers.importStep(str(output)).val()
    assert shape.isValid() and len(shape.Solids()) == 33
    print(f"{output}: valid STEP, {len(shape.Solids())} solids, one shared housing")


if __name__ == "__main__":
    main()
