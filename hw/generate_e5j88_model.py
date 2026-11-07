#!/usr/bin/env python3
"""Rebuild the E5J88-00LJG2-L STEP model using CadQuery 2.8.

Run with CadQuery installed: python hw/generate_e5j88_model.py
Use --output /tmp/connector.step to export without changing project assets.
Source: Pulse CE5J88-00LJGX-L Rev 0 (2013-03-20), linked in the footprint README.
All dimensions are millimeters; Z=0 is the PCB top. KiCad model Y is opposite
footprint Y, so the cable opening faces positive model Y. Undimensioned socket
details and retention clips are visual approximations.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil

import cadquery as cq


MODEL_NAME = "RJ45_Pulse_E5J88-00LJG2-L_Horizontal"
BODY_WIDTH, BODY_DEPTH, BODY_HEIGHT = 15.80, 15.00, 12.60
STANDOFF, CORNER_RADIUS = 0.50, 0.50
PIN_PITCH, ROW_SPACING, PIN_THICKNESS, PIN_PROTRUSION = 1.02, 1.78, 0.35, 2.67
PEG_SPACING, PEG_FRONT_OFFSET = 12.70, 5.10
CENTER_X = 3.5 * PIN_PITCH
PEG_Y = 4.32 - ROW_SPACING
FRONT_Y = PEG_Y + PEG_FRONT_OFFSET
REAR_Y = FRONT_Y - BODY_DEPTH


def block(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY")
            .box(x1-x0, y1-y0, z1-z0, centered=False)
            .translate((x0, y0, z0)).val())


def contact(x, pin_y):
    """Approximate the internal stamped contact with the drawn 0.35 mm stock."""
    half = PIN_THICKNESS / 2
    bridge = block(x-half, x+half, -3.125, pin_y+half, 1.15, 1.50)
    bend = block(x-half, x+half, -3.125, -2.775, 1.15, 5.20)
    spring = (cq.Workplane("YZ", origin=(x-half, 0, 0))
              .polyline([(-3.125, 4.85), (6.50, 2.60),
                         (6.50, 2.95), (-3.125, 5.20)])
              .close().extrude(PIN_THICKNESS).val())
    return bridge.fuse(bend, spring).clean()


def build_model():
    x0, x1 = CENTER_X-BODY_WIDTH/2, CENTER_X+BODY_WIDTH/2
    body = (cq.Workplane("XY")
            .box(BODY_WIDTH, BODY_DEPTH, BODY_HEIGHT, centered=False)
            .translate((x0, REAR_Y, STANDOFF))
            .edges("|Y and >Z").fillet(CORNER_RADIUS).val())

    # The stepped mouth follows the front elevation; its depth is not specified.
    mouth = [(-6.50, 2.20), (6.50, 2.20), (6.50, 9.80), (3.85, 9.80),
             (3.85, 11.40), (2.05, 11.40), (2.05, 12.70),
             (-2.05, 12.70), (-2.05, 11.40), (-3.85, 11.40),
             (-3.85, 9.80), (-6.50, 9.80)]
    mouth_plane = cq.Plane(origin=(CENTER_X, FRONT_Y+0.10, 0),
                           xDir=(1, 0, 0), normal=(0, -1, 0))
    cavity = (cq.Workplane(mouth_plane).polyline(mouth).close()
              .extrude(10.90).val())
    body = body.cut(cavity)

    # Rear feet establish the dimensioned 0.50 mm body standoff.
    for a, b in ((x0, x0+2.0), (x1-2.0, x1)):
        body = body.fuse(block(a, b, REAR_Y, REAR_Y+4.0, 0, STANDOFF+0.1))

    # Clip contours are approximate and fit inside the specified 3.20 mm holes.
    for x in (CENTER_X-PEG_SPACING/2, CENTER_X+PEG_SPACING/2):
        peg = (cq.Workplane("XY").center(x, PEG_Y).circle(1.50)
               .extrude(4.15).translate((0, 0, -3.50))
               .edges("<Z").chamfer(0.35).val())
        slit = block(x-0.30, x+0.30, PEG_Y-2.0, PEG_Y+2.0, -3.60, -0.20)
        body = body.fuse(peg.cut(slit))

    terminals, contacts = [], []
    half = PIN_THICKNESS / 2
    for index in range(8):
        x = index * PIN_PITCH
        y = -ROW_SPACING if index % 2 else 0
        terminals.append(block(x-half, x+half, y-half, y+half,
                               -PIN_PROTRUSION, 1.50))
        contacts.append(contact(x, y))

    # Remove embedded metal volumes so distinct material solids do not overlap.
    metal = cq.Compound.makeCompound(terminals + contacts)
    body = body.cut(metal).clean()
    assert body.isValid() and len(body.Solids()) == 1
    bounds = body.BoundingBox()
    for actual, expected in ((bounds.xlen, BODY_WIDTH), (bounds.ylen, BODY_DEPTH),
                             (bounds.zmax, STANDOFF+BODY_HEIGHT)):
        assert abs(actual-expected) < 1e-6, (actual, expected)

    assembly = cq.Assembly(name=MODEL_NAME)
    assembly.add(body, name="black_nylon_housing", color=cq.Color(0.10, 0.105, 0.115))
    for index, (terminal, spring) in enumerate(zip(terminals, contacts)):
        assert terminal.isValid() and spring.isValid()
        bounds = terminal.BoundingBox()
        assert abs(bounds.zmin + PIN_PROTRUSION) < 1e-6
        assert abs((bounds.xmin+bounds.xmax)/2-index*PIN_PITCH) < 1e-6
        expected_y = -ROW_SPACING if index % 2 else 0
        assert abs((bounds.ymin+bounds.ymax)/2-expected_y) < 1e-6
        assembly.add(terminal, name=f"pin_{index+1}_tin_tail",
                     color=cq.Color(0.72, 0.74, 0.77))
        # The overlap with the tail is assigned to the tin-plated region.
        assembly.add(spring.cut(terminal).clean(), name=f"pin_{index+1}_gold_contact",
                     color=cq.Color(0.83, 0.64, 0.20))
    return assembly


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Export one STEP to this path")
    args = parser.parse_args()
    if args.output:
        outputs = [args.output]
    else:
        hardware = Path(__file__).resolve().parent
        outputs = [hardware / project / "models" / (MODEL_NAME + ".step")
                   for project in ("octopus", "tentacle")]
    assembly = build_model()
    outputs[0].parent.mkdir(parents=True, exist_ok=True)
    assembly.export(str(outputs[0]), exportType="STEP", unit="MM")
    shape = cq.importers.importStep(str(outputs[0])).val()
    assert shape.isValid() and len(shape.Solids()) == 17
    for output in outputs[1:]:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(outputs[0], output)
    for output in outputs:
        print(f"{output}: valid STEP, {len(shape.Solids())} solids")


if __name__ == "__main__":
    main()
