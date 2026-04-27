#!/usr/bin/env python3
"""Generate printable STL files for the SG98 back-cover remote case.

This avoids relying on OpenSCAD on macOS.
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

import mapbox_earcut as earcut
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
CASE_DIR = (
    ROOT
    / "Documentation"
    / "Hardware"
    / "waveshare-esp32-s3-touch-amoled-1.75"
    / "case"
)
OUT_DIR = CASE_DIR / "print_files"
PREVIEW_OUT_DIR = CASE_DIR / "preview_files"


def pt(cx: float, cy: float, r: float, deg: float) -> tuple[float, float]:
    a = math.radians(deg)
    return cx + math.cos(a) * r, cy + math.sin(a) * r


def loop_area(loop: list[tuple[float, float]]) -> float:
    total = 0.0
    for i, p in enumerate(loop):
        q = loop[(i + 1) % len(loop)]
        total += p[0] * q[1] - q[0] * p[1]
    return total * 0.5


def ccw(loop: list[tuple[float, float]]) -> list[tuple[float, float]]:
    return loop if loop_area(loop) > 0 else list(reversed(loop))


def cw(loop: list[tuple[float, float]]) -> list[tuple[float, float]]:
    return loop if loop_area(loop) < 0 else list(reversed(loop))


def rounded_rect_loop(
    cx: float,
    cy: float,
    w: float,
    h: float,
    r: float,
    segments: int = 8,
) -> list[tuple[float, float]]:
    r = min(r, w / 2, h / 2)
    corners = [
        (cx + w / 2 - r, cy + h / 2 - r, 0, 90),
        (cx - w / 2 + r, cy + h / 2 - r, 90, 180),
        (cx - w / 2 + r, cy - h / 2 + r, 180, 270),
        (cx + w / 2 - r, cy - h / 2 + r, 270, 360),
    ]
    points: list[tuple[float, float]] = []
    for x, y, start, end in corners:
        for i in range(segments + 1):
            if points and i == 0:
                continue
            points.append(pt(x, y, r, start + (end - start) * i / segments))
    return ccw(points)


def circle_loop(cx: float, cy: float, r: float, segments: int = 48) -> list[tuple[float, float]]:
    return ccw([pt(cx, cy, r, 360 * i / segments) for i in range(segments)])


class Mesh:
    def __init__(self, name: str) -> None:
        self.name = name
        self.triangles: list[tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]] = []

    def tri(self, a: tuple[float, float, float], b: tuple[float, float, float], c: tuple[float, float, float]) -> None:
        self.triangles.append((a, b, c))

    def quad(
        self,
        a: tuple[float, float, float],
        b: tuple[float, float, float],
        c: tuple[float, float, float],
        d: tuple[float, float, float],
    ) -> None:
        self.tri(a, b, c)
        self.tri(a, c, d)

    def extend(self, other: "Mesh") -> None:
        self.triangles.extend(other.triangles)

    def translate(self, dx: float, dy: float, dz: float) -> None:
        moved = []
        for tri in self.triangles:
            moved.append(tuple((p[0] + dx, p[1] + dy, p[2] + dz) for p in tri))  # type: ignore[arg-type]
        self.triangles = moved  # type: ignore[assignment]

    def print_center(self) -> None:
        xs = [p[0] for tri in self.triangles for p in tri]
        ys = [p[1] for tri in self.triangles for p in tri]
        zs = [p[2] for tri in self.triangles for p in tri]
        self.translate(-(min(xs) + max(xs)) / 2, -(min(ys) + max(ys)) / 2, -min(zs))

    def write_binary_stl(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        header = f"SG98 {self.name}".encode("ascii", "ignore")[:80].ljust(80, b" ")
        with path.open("wb") as f:
            f.write(header)
            f.write(struct.pack("<I", len(self.triangles)))
            for a, b, c in self.triangles:
                n = normal(a, b, c)
                f.write(struct.pack("<12fH", *n, *a, *b, *c, 0))


def normal(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
    c: tuple[float, float, float],
) -> tuple[float, float, float]:
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    mag = math.sqrt(nx * nx + ny * ny + nz * nz)
    if mag <= 1e-9:
        return (0.0, 0.0, 0.0)
    return (nx / mag, ny / mag, nz / mag)


def extrude_polygon(
    mesh: Mesh,
    loops: list[list[tuple[float, float]]],
    z0: float,
    z1: float,
) -> None:
    prepared = [ccw(loops[0])] + [cw(loop) for loop in loops[1:]]
    vertices: list[tuple[float, float]] = []
    ends: list[int] = []
    for loop in prepared:
        vertices.extend(loop)
        ends.append(len(vertices))

    tris = earcut.triangulate_float64(np.array(vertices, dtype=np.float64), np.array(ends, dtype=np.uint32))
    for i in range(0, len(tris), 3):
        ia, ib, ic = int(tris[i]), int(tris[i + 1]), int(tris[i + 2])
        a, b, c = vertices[ia], vertices[ib], vertices[ic]
        mesh.tri((a[0], a[1], z1), (b[0], b[1], z1), (c[0], c[1], z1))
        mesh.tri((c[0], c[1], z0), (b[0], b[1], z0), (a[0], a[1], z0))

    for ring_i, loop in enumerate(prepared):
        reverse = ring_i > 0
        for i, p in enumerate(loop):
            q = loop[(i + 1) % len(loop)]
            a, b = (p[0], p[1], z0), (q[0], q[1], z0)
            c, d = (q[0], q[1], z1), (p[0], p[1], z1)
            if reverse:
                mesh.quad(a, d, c, b)
            else:
                mesh.quad(a, b, c, d)


def add_box(mesh: Mesh, x0: float, x1: float, y0: float, y1: float, z0: float, z1: float) -> None:
    if x1 <= x0 or y1 <= y0 or z1 <= z0:
        return
    p000 = (x0, y0, z0)
    p100 = (x1, y0, z0)
    p110 = (x1, y1, z0)
    p010 = (x0, y1, z0)
    p001 = (x0, y0, z1)
    p101 = (x1, y0, z1)
    p111 = (x1, y1, z1)
    p011 = (x0, y1, z1)
    mesh.quad(p000, p010, p110, p100)
    mesh.quad(p001, p101, p111, p011)
    mesh.quad(p000, p100, p101, p001)
    mesh.quad(p100, p110, p111, p101)
    mesh.quad(p110, p010, p011, p111)
    mesh.quad(p010, p000, p001, p011)


def extrude_xz_polygon_y(
    mesh: Mesh,
    loops: list[list[tuple[float, float]]],
    y0: float,
    y1: float,
) -> None:
    """Extrude x/z-plane loops through a y-thickness."""
    prepared = [ccw(loops[0])] + [cw(loop) for loop in loops[1:]]
    vertices: list[tuple[float, float]] = []
    ends: list[int] = []
    for loop in prepared:
        vertices.extend(loop)
        ends.append(len(vertices))

    tris = earcut.triangulate_float64(np.array(vertices, dtype=np.float64), np.array(ends, dtype=np.uint32))
    for i in range(0, len(tris), 3):
        ia, ib, ic = int(tris[i]), int(tris[i + 1]), int(tris[i + 2])
        a, b, c = vertices[ia], vertices[ib], vertices[ic]
        mesh.tri((a[0], y1, a[1]), (b[0], y1, b[1]), (c[0], y1, c[1]))
        mesh.tri((c[0], y0, c[1]), (b[0], y0, b[1]), (a[0], y0, a[1]))

    for ring_i, loop in enumerate(prepared):
        reverse = ring_i > 0
        for i, p in enumerate(loop):
            q = loop[(i + 1) % len(loop)]
            a, b = (p[0], y0, p[1]), (q[0], y0, q[1])
            c, d = (q[0], y1, q[1]), (p[0], y1, p[1])
            if reverse:
                mesh.quad(a, d, c, b)
            else:
                mesh.quad(a, b, c, d)


def add_y_wall_with_snap_slots(
    mesh: Mesh,
    x0: float,
    x1: float,
    y0: float,
    y1: float,
    z0: float,
    z1: float,
    slot_centers: list[float],
    slot_len: float,
    slot_z0: float,
    slot_z1: float,
) -> None:
    """Add an x-thickness wall with rectangular side snap receiver slots."""
    add_box(mesh, x0, x1, y0, y1, z0, slot_z0)
    add_box(mesh, x0, x1, y0, y1, slot_z1, z1)

    cuts = sorted((c - slot_len / 2, c + slot_len / 2) for c in slot_centers)
    cursor = y0
    for cut0, cut1 in cuts:
        if cut0 > cursor:
            add_box(mesh, x0, x1, cursor, cut0, slot_z0, slot_z1)
        cursor = max(cursor, cut1)
    if cursor < y1:
        add_box(mesh, x0, x1, cursor, y1, slot_z0, slot_z1)


def add_annular_sector(
    mesh: Mesh,
    cx: float,
    cy: float,
    outer_r: float,
    inner_r: float,
    start_deg: float,
    end_deg: float,
    z0: float,
    z1: float,
    segments: int = 40,
) -> None:
    outer = [pt(cx, cy, outer_r, start_deg + (end_deg - start_deg) * i / segments) for i in range(segments + 1)]
    inner = [pt(cx, cy, inner_r, end_deg - (end_deg - start_deg) * i / segments) for i in range(segments + 1)]
    extrude_polygon(mesh, [ccw(outer + inner)], z0, z1)


def backcover_dimensions() -> dict[str, float]:
    base_w = 44.0
    base_l = 70.0
    shell_h = 15.0
    wall = 2.0
    deck_thickness = 8.0
    remote_od = 48.96
    remote_clearance = 0.45
    remote_cavity_r = (remote_od + 2 * remote_clearance) / 2
    remote_seat_depth = 7.2
    remote_seat_r = remote_cavity_r + 0.9
    remote_snap_outer_r = remote_cavity_r + 3.4
    wire_window_r = 17.5
    magnet_w = 10.0
    magnet_l = 60.0
    magnet_h = 1.5
    magnet_clearance = 0.35
    return {
        "base_w": base_w,
        "base_l": base_l,
        "shell_h": shell_h,
        "wall": wall,
        "deck_z0": shell_h - deck_thickness,
        "remote_cavity_r": remote_cavity_r,
        "remote_seat_depth": remote_seat_depth,
        "remote_seat_r": remote_seat_r,
        "remote_snap_outer_r": remote_snap_outer_r,
        "wire_window_r": wire_window_r,
        "magnet_pocket_w": magnet_w + 2 * magnet_clearance,
        "magnet_pocket_l": magnet_l + 2 * magnet_clearance,
        "magnet_h": magnet_h,
        "lid_floor_h": 1.0,
        "lid_h": 2.8,
        "lid_lip_h": 1.2,
        "pressure_bay_w": 22.0,
        "pressure_bay_h": 10.0,
        "pressure_bay_z": 7.1,
        "pressure_bay_recess_depth": 4.0,
        "pressure_bay_back_thickness": 0.85,
        "pressure_connector_hole_d": 5.8,
    }


def _arc_points(
    cx: float,
    cy: float,
    r: float,
    start_deg: float,
    end_deg: float,
    segments: int,
) -> list[tuple[float, float]]:
    return [pt(cx, cy, r, start_deg + (end_deg - start_deg) * i / segments) for i in range(segments + 1)]


def open_round_socket_cap_loops(d: dict[str, float]) -> list[list[tuple[float, float]]]:
    # The lower case remains narrower than the remote so the USB-C/buttons stay
    # exposed, but the actual locating pocket must still be round. The circular
    # recess therefore opens through the left/right sides of the upper deck.
    bw, bl = d["base_w"], d["base_l"]
    half_w, half_l = bw / 2, bl / 2
    corner_r = 7.0
    pocket_r = d["remote_cavity_r"] + 0.25
    y_int = math.sqrt(max(0.0, pocket_r * pocket_r - half_w * half_w))
    alpha = math.degrees(math.atan2(y_int, half_w))

    top_loop: list[tuple[float, float]] = [
        (-half_w, y_int),
        (-half_w, half_l - corner_r),
        *_arc_points(-half_w + corner_r, half_l - corner_r, corner_r, 180, 90, 10)[1:],
        (half_w - corner_r, half_l),
        *_arc_points(half_w - corner_r, half_l - corner_r, corner_r, 90, 0, 10)[1:],
        (half_w, y_int),
        *_arc_points(0, 0, pocket_r, alpha, 180 - alpha, 36)[1:],
    ]

    bottom_loop: list[tuple[float, float]] = [
        (half_w, -y_int),
        (half_w, -half_l + corner_r),
        *_arc_points(half_w - corner_r, -half_l + corner_r, corner_r, 0, -90, 10)[1:],
        (-half_w + corner_r, -half_l),
        *_arc_points(-half_w + corner_r, -half_l + corner_r, corner_r, -90, -180, 10)[1:],
        (-half_w, -y_int),
        *_arc_points(0, 0, pocket_r, 180 + alpha, 360 - alpha, 36)[1:],
    ]

    return [ccw(top_loop), ccw(bottom_loop)]


def build_backcover_shell() -> Mesh:
    d = backcover_dimensions()
    mesh = Mesh("sg98_remote_backcover_shell")
    bw, bl, wall = d["base_w"], d["base_l"], d["wall"]
    shell_h, deck_z0 = d["shell_h"], d["deck_z0"]
    xmin, xmax = -bw / 2, bw / 2
    ymin, ymax = -bl / 2, bl / 2
    ixmin, ixmax = xmin + wall, xmax - wall
    iymin, iymax = ymin + wall, ymax - wall
    corner_r = 7.0
    inner_corner_r = corner_r - wall

    # Top surface: an open-sided round socket locates the remote, while the
    # central window leaves room for wiring on the back of the board.
    deck = rounded_rect_loop(0, 0, bw, bl, 7.0)
    wire_window = circle_loop(0, 0, d["wire_window_r"], 64)
    seat_z = shell_h - d["remote_seat_depth"]
    extrude_polygon(mesh, [deck, wire_window], deck_z0, seat_z)
    for cap_loop in open_round_socket_cap_loops(d):
        extrude_polygon(mesh, [cap_loop], seat_z, shell_h)

    # Side walls below the deck. Straight wall spans are joined by true rounded
    # corner sectors, so the lower shell follows the same corner curve as the top.
    add_y_wall_with_snap_slots(mesh, xmin, ixmin, ymin + corner_r, ymax - corner_r, 0, deck_z0, [-18.0, 18.0], 8.5, 2.4, 4.6)
    add_y_wall_with_snap_slots(mesh, ixmax, xmax, ymin + corner_r, ymax - corner_r, 0, deck_z0, [-18.0, 18.0], 8.5, 2.4, 4.6)
    add_box(mesh, xmin + corner_r, xmax - corner_r, ymin, iymin, 0, deck_z0)

    for cx, cy, start, end in [
        (xmax - corner_r, ymax - corner_r, 0, 90),
        (xmin + corner_r, ymax - corner_r, 90, 180),
        (xmin + corner_r, ymin + corner_r, 180, 270),
        (xmax - corner_r, ymin + corner_r, 270, 360),
    ]:
        add_annular_sector(mesh, cx, cy, corner_r, inner_corner_r, start, end, 0, deck_z0, 18)

    end_wall_z1 = max(deck_z0, d["pressure_bay_z"] + d["pressure_bay_h"] / 2 + 1.0)
    end_wall = [
        (xmin + corner_r, 0.0),
        (xmax - corner_r, 0.0),
        (xmax - corner_r, end_wall_z1),
        (xmin + corner_r, end_wall_z1),
    ]
    pressure_bay = rounded_rect_loop(
        0,
        d["pressure_bay_z"],
        d["pressure_bay_w"],
        d["pressure_bay_h"],
        d["pressure_bay_h"] / 2,
        18,
    )
    pressure_bay_boss = rounded_rect_loop(
        0,
        d["pressure_bay_z"],
        d["pressure_bay_w"] + 4.2,
        d["pressure_bay_h"] + 4.2,
        (d["pressure_bay_h"] + 4.2) / 2,
        18,
    )
    connector_hole = circle_loop(0, d["pressure_bay_z"], d["pressure_connector_hole_d"] / 2, 48)
    bay_back_y = ymax - d["pressure_bay_recess_depth"]
    extrude_xz_polygon_y(mesh, [end_wall, pressure_bay], iymax, ymax)
    extrude_xz_polygon_y(mesh, [pressure_bay_boss, pressure_bay], bay_back_y, iymax)
    extrude_xz_polygon_y(mesh, [pressure_bay, connector_hole], bay_back_y, bay_back_y + d["pressure_bay_back_thickness"])

    mesh.print_center()
    return mesh


def build_backcover_lid() -> Mesh:
    d = backcover_dimensions()
    mesh = Mesh("sg98_remote_backcover_bottom_lid")
    bw, bl = d["base_w"], d["base_l"]
    lid_floor_h, lid_h, lip_h = d["lid_floor_h"], d["lid_h"], d["lid_lip_h"]
    wall = d["wall"]
    plate = rounded_rect_loop(0, 0, bw - 0.2, bl - 0.2, 7.0)
    lip_outer = rounded_rect_loop(0, 0, bw - 2 * wall - 0.45, bl - 2 * wall - 0.45, 5.2)
    lip_inner = rounded_rect_loop(0, 0, bw - 2 * wall - 5.5, bl - 2 * wall - 5.5, 4.2)
    pocket_w = d["magnet_pocket_w"]
    pocket_l = d["magnet_pocket_l"]
    pocket = rounded_rect_loop(0, 0, pocket_w, pocket_l, 1.3)

    # Print orientation: smooth outside bottom on the bed. The magnet is
    # recessed into the inside face with a plastic floor below it.
    extrude_polygon(mesh, [plate], 0.0, lid_floor_h)
    extrude_polygon(mesh, [plate, pocket], lid_floor_h, lid_h)
    extrude_polygon(mesh, [lip_outer, lip_inner], lid_h, lid_h + lip_h)

    rail = 1.0
    z0 = lid_h
    z1 = lid_h + 0.65
    add_box(mesh, -pocket_w / 2 - rail, -pocket_w / 2, -pocket_l / 2, pocket_l / 2, z0, z1)
    add_box(mesh, pocket_w / 2, pocket_w / 2 + rail, -pocket_l / 2, pocket_l / 2, z0, z1)
    add_box(mesh, -pocket_w / 2 - rail, pocket_w / 2 + rail, -pocket_l / 2 - rail, -pocket_l / 2, z0, z1)
    add_box(mesh, -pocket_w / 2 - rail, pocket_w / 2 + rail, pocket_l / 2, pocket_l / 2 + rail, z0, z1)

    # Four snap tabs on the lid lip. They flex into the side-wall receiver slots
    # in the shell. Sand these down slightly if a first print is too tight.
    lip_outer_w = bw - 2 * wall - 0.45
    tab_x0 = lip_outer_w / 2 - 0.2
    tab_x1 = lip_outer_w / 2 + 1.0
    tab_len = 7.6
    tab_z0 = lid_h + 0.35
    tab_z1 = lid_h + 1.05
    for y in [-18.0, 18.0]:
        add_box(mesh, tab_x0, tab_x1, y - tab_len / 2, y + tab_len / 2, tab_z0, tab_z1)
        add_box(mesh, -tab_x1, -tab_x0, y - tab_len / 2, y + tab_len / 2, tab_z0, tab_z1)

    mesh.print_center()
    return mesh


def build_pressure_socket_cover() -> Mesh:
    d = backcover_dimensions()
    mesh = Mesh("sg98_pressure_socket_snap_cover")
    bay_w, bay_h = d["pressure_bay_w"], d["pressure_bay_h"]

    face = rounded_rect_loop(0, 0, bay_w - 0.8, bay_h - 0.8, (bay_h - 0.8) / 2, 18)
    plug = rounded_rect_loop(0, 0, bay_w - 2.7, bay_h - 2.6, (bay_h - 2.6) / 2, 16)

    # The thin face sits nearly flush with the end of the case; the inner boss
    # friction-fits into the recessed bay. Print in PETG or TPU for the best snap.
    extrude_xz_polygon_y(mesh, [face], 0.0, 1.2)
    extrude_xz_polygon_y(mesh, [plug], 1.2, 3.1)

    # Small side bumps add light retention against the bay walls.
    bump_z = 1.8
    add_box(mesh, -bay_w / 2 - 0.15, -bay_w / 2 + 0.55, 1.7, 2.85, -bump_z, bump_z)
    add_box(mesh, bay_w / 2 - 0.55, bay_w / 2 + 0.15, 1.7, 2.85, -bump_z, bump_z)

    # A tiny raised lip gives a fingernail a place to pull the cover out.
    add_box(mesh, -4.6, 4.6, -0.45, 0.0, 2.7, 3.25)

    mesh.print_center()
    return mesh


def build_backcover_fit_check_assembly() -> Mesh:
    d = backcover_dimensions()
    assembly = Mesh("sg98_remote_backcover_fit_check_assembly_not_for_print")
    assembly.extend(build_backcover_shell())

    # Dummy remote body seated in the shell. This is for visualization only and
    # intentionally intersects the socket like the real board would occupy it.
    remote = Mesh("dummy_remote")
    remote_z0 = d["shell_h"] - d["remote_seat_depth"]
    extrude_polygon(remote, [circle_loop(0, 0, 48.96 / 2, 96)], remote_z0, remote_z0 + 10.4)
    extrude_polygon(remote, [circle_loop(0, 0, 44.16 / 2, 96)], remote_z0 + 10.4, remote_z0 + 11.4)
    assembly.extend(remote)
    return assembly


def main() -> None:
    outputs = [
        (build_backcover_shell(), "sg98_remote_backcover_shell.stl"),
        (build_backcover_lid(), "sg98_remote_backcover_bottom_lid.stl"),
        (build_pressure_socket_cover(), "sg98_pressure_socket_snap_cover.stl"),
    ]
    for mesh, filename in outputs:
        path = OUT_DIR / filename
        mesh.write_binary_stl(path)
        print(f"{path}  triangles={len(mesh.triangles)}")

    preview_outputs = [
        (build_backcover_fit_check_assembly(), "sg98_remote_backcover_fit_check_assembly_NOT_FOR_PRINT.stl"),
    ]
    for mesh, filename in preview_outputs:
        path = PREVIEW_OUT_DIR / filename
        mesh.write_binary_stl(path)
        print(f"{path}  triangles={len(mesh.triangles)}")


if __name__ == "__main__":
    main()
