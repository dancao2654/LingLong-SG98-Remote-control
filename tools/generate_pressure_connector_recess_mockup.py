#!/usr/bin/env python3
"""Generate a visual mockup of the recessed pressure connector bay."""

from __future__ import annotations

from pathlib import Path

from generate_remote_case_preview import Svg, rounded_rect_path, tag


ROOT = Path(__file__).resolve().parents[1]
CASE_DIR = (
    ROOT
    / "Documentation"
    / "Hardware"
    / "waveshare-esp32-s3-touch-amoled-1.75"
    / "case"
)
OUT = CASE_DIR / "sg98_pressure_connector_recess_mockup.svg"


def add_defs() -> list[str]:
    return [
        "<defs>",
        '<linearGradient id="caseFace" x1="0" x2="1" y1="0" y2="1">',
        '<stop offset="0%" stop-color="#f8fafc" />',
        '<stop offset="100%" stop-color="#cbd5e1" />',
        "</linearGradient>",
        '<linearGradient id="pocket" x1="0" x2="0" y1="0" y2="1">',
        '<stop offset="0%" stop-color="#dbe4ee" />',
        '<stop offset="100%" stop-color="#aebccc" />',
        "</linearGradient>",
        '<linearGradient id="metal" x1="0" x2="1" y1="0" y2="1">',
        '<stop offset="0%" stop-color="#f8fafc" />',
        '<stop offset="48%" stop-color="#94a3b8" />',
        '<stop offset="100%" stop-color="#475569" />',
        "</linearGradient>",
        '<marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">',
        '<path d="M0,0 L0,6 L9,3 z" fill="#475569" />',
        "</marker>",
        "</defs>",
    ]


def line_arrow(s: Svg, x1: float, y1: float, x2: float, y2: float, label: str) -> None:
    s.add(
        "line",
        {
            "x1": x1,
            "y1": y1,
            "x2": x2,
            "y2": y2,
            "stroke": "#475569",
            "stroke-width": 2,
            "stroke-linecap": "round",
            "marker-end": "url(#arrow)",
        },
    )
    s.text(x1, y1 - 10, label, 15, "#334155", "700")


def main() -> None:
    s = Svg()
    width = 1600
    height = 1000

    s.text(54, 72, "Recessed Pressure Connector Mockup", 30, "#111827", "700")
    s.text(54, 105, "M5 female socket sits behind the case lip; optional snap cover closes the bay when the sensor is unplugged.", 15, "#64748b")

    # Front/end view of the short side of the case.
    fx, fy = 92, 205
    fw, fh = 610, 270
    s.text(fx, fy - 32, "Short-End View", 21, "#111827", "700")
    s.path(rounded_rect_path(fx, fy, fw, fh, 46), "url(#caseFace)", "#334155", 3)
    s.path(rounded_rect_path(fx + 155, fy + 70, 300, 136, 58), "url(#pocket)", "#64748b", 2.4)
    s.path(rounded_rect_path(fx + 168, fy + 83, 274, 110, 48), "#eef4fb", "#cbd5e1", 1.3)
    s.circle(fx + 305, fy + 138, 52, "url(#metal)", "#475569", 2)
    s.circle(fx + 305, fy + 138, 37, "#111827", "#020617", 2)
    for dx, dy in [(-14, -10), (16, -10), (0, 16)]:
        s.circle(fx + 305 + dx, fy + 138 + dy, 5.4, "#e2e8f0", "#64748b", 1.1)
    s.text(fx + 305, fy + 235, "connector face is protected inside the recess", 15, "#334155", "700", "middle")

    line_arrow(s, fx + 128, fy + 76, fx + 188, fy + 103, "outer case lip")
    line_arrow(s, fx + 560, fy + 75, fx + 404, fy + 123, "recessed M5 socket")

    # Side cutaway.
    sx, sy = 820, 215
    s.text(sx, sy - 42, "Side Cutaway", 21, "#111827", "700")
    s.rect(sx + 70, sy + 70, 500, 170, "url(#caseFace)", "#334155", 2.4, 20)
    s.rect(sx + 70, sy + 105, 70, 100, "#cbd5e1", "#334155", 2, 14)
    s.rect(sx + 140, sy + 123, 135, 64, "#b7c4d3", "#64748b", 1.8, 22)
    s.rect(sx + 255, sy + 134, 22, 42, "url(#metal)", "#475569", 1.7, 4)
    s.rect(sx + 277, sy + 125, 92, 60, "url(#metal)", "#475569", 1.7, 11)
    s.rect(sx + 369, sy + 142, 78, 26, "#111827", "#020617", 1.5, 9)
    s.rect(sx + 447, sy + 147, 180, 16, "#111827", "#020617", 1.2, 8)
    s.text(sx + 213, sy + 218, "4 mm recess", 14, "#334155", "700", "middle")
    s.text(sx + 334, sy + 112, "panel nut / body inside case", 14, "#334155", "700", "middle")
    s.text(sx + 540, sy + 194, "sensor cable plug", 14, "#334155", "700", "middle")
    line_arrow(s, sx + 78, sy + 52, sx + 122, sy + 112, "front lip")
    line_arrow(s, sx + 505, sy + 66, sx + 384, sy + 144, "plug enters bay")

    # Snap cover state.
    cx, cy = 240, 675
    s.text(92, 610, "Sensor Not Attached", 21, "#111827", "700")
    s.path(rounded_rect_path(92, 650, 610, 190, 38), "url(#caseFace)", "#334155", 2.6)
    s.path(rounded_rect_path(222, 695, 350, 94, 46), "url(#pocket)", "#64748b", 2.2)
    s.path(rounded_rect_path(245, 710, 304, 64, 30), "#e7eef7", "#475569", 2.2)
    s.rect(385, 704, 12, 76, "#94a3b8", "#475569", 1.2, 4)
    s.text(397, 815, "snap-in dust cover sits flush in the same bay", 15, "#334155", "700", "middle")
    s.text(282, 744, "small pull lip", 13, "#64748b", "700", "middle")
    line_arrow(s, 700, 685, 546, 721, "cover snaps into pocket walls")

    # Wiring callout.
    wx, wy = 900, 640
    s.text(wx, wy - 30, "Internal Wiring", 21, "#111827", "700")
    s.rect(wx, wy, 470, 168, "#ffffff", "#cbd5e1", 1.6, 18)
    rows = [
        ("Pin 1 / Brown", "SUP / +5V", "#dc2626"),
        ("Pin 3 / Blue", "GND", "#2563eb"),
        ("Pin 4 / Black", "OUT / pressure signal", "#111827"),
    ]
    for i, (pin, name, color) in enumerate(rows):
        y = wy + 40 + i * 42
        s.circle(wx + 36, y - 5, 7, color, color, 1)
        s.text(wx + 60, y, pin, 15, "#334155", "700")
        s.text(wx + 210, y, name, 15, color, "700")
    s.text(wx + 36, wy + 145, "Female connector on case, male plug on sensor cable.", 14, "#64748b", "600")

    svg = tag(
        "svg",
        {
            "xmlns": "http://www.w3.org/2000/svg",
            "width": width,
            "height": height,
            "viewBox": f"0 0 {width} {height}",
        },
        "\n".join(
            [
                *add_defs(),
                f'<rect width="{width}" height="{height}" fill="#f6f7f9" />',
                *s.items,
            ]
        ),
    )
    OUT.write_text(svg + "\n", encoding="utf-8")
    print(OUT)


if __name__ == "__main__":
    main()
