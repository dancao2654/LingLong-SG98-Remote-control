#!/usr/bin/env python3
"""Generate an SVG/PNG-friendly visual preview for the SG98 back-cover case."""

from __future__ import annotations

import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CASE_DIR = (
    ROOT
    / "Documentation"
    / "Hardware"
    / "waveshare-esp32-s3-touch-amoled-1.75"
    / "case"
)
OUT = CASE_DIR / "sg98_remote_case_preview.svg"


def esc(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def tag(name: str, attrs: dict[str, object] | None = None, body: str | None = None) -> str:
    attrs = attrs or {}
    rendered_attrs = " ".join(f'{key}="{esc(str(value))}"' for key, value in attrs.items())
    prefix = f"<{name}{(' ' + rendered_attrs) if rendered_attrs else ''}"
    if body is None:
        return prefix + " />"
    return prefix + f">{body}</{name}>"


class Svg:
    def __init__(self) -> None:
        self.items: list[str] = []

    def add(self, name: str, attrs: dict[str, object] | None = None, body: str | None = None) -> None:
        self.items.append(tag(name, attrs, body))

    def text(
        self,
        x: float,
        y: float,
        text: str,
        size: int = 14,
        fill: str = "#20242a",
        weight: str = "500",
        anchor: str = "start",
    ) -> None:
        self.add(
            "text",
            {
                "x": round(x, 2),
                "y": round(y, 2),
                "font-size": size,
                "font-weight": weight,
                "fill": fill,
                "text-anchor": anchor,
                "font-family": "Inter, Helvetica, Arial, sans-serif",
            },
            esc(text),
        )

    def line(self, x1: float, y1: float, x2: float, y2: float, stroke: str = "#64748b", width: float = 1.5) -> None:
        self.add(
            "line",
            {
                "x1": round(x1, 2),
                "y1": round(y1, 2),
                "x2": round(x2, 2),
                "y2": round(y2, 2),
                "stroke": stroke,
                "stroke-width": width,
                "stroke-linecap": "round",
            },
        )

    def circle(self, cx: float, cy: float, r: float, fill: str, stroke: str = "#111827", width: float = 1.4) -> None:
        self.add(
            "circle",
            {
                "cx": round(cx, 2),
                "cy": round(cy, 2),
                "r": round(r, 2),
                "fill": fill,
                "stroke": stroke,
                "stroke-width": width,
            },
        )

    def rect(
        self,
        x: float,
        y: float,
        w: float,
        h: float,
        fill: str,
        stroke: str = "#111827",
        width: float = 1.4,
        rx: float = 0,
        transform: str = "",
    ) -> None:
        attrs: dict[str, object] = {
            "x": round(x, 2),
            "y": round(y, 2),
            "width": round(w, 2),
            "height": round(h, 2),
            "fill": fill,
            "stroke": stroke,
            "stroke-width": width,
            "rx": rx,
        }
        if transform:
            attrs["transform"] = transform
        self.add("rect", attrs)

    def path(self, d: str, fill: str, stroke: str = "#111827", width: float = 1.4) -> None:
        self.add(
            "path",
            {
                "d": d,
                "fill": fill,
                "stroke": stroke,
                "stroke-width": width,
                "stroke-linejoin": "round",
            },
        )


def point(cx: float, cy: float, r: float, deg: float) -> tuple[float, float]:
    rad = math.radians(deg)
    return cx + math.cos(rad) * r, cy + math.sin(rad) * r


def rounded_rect_path(x: float, y: float, w: float, h: float, r: float) -> str:
    return (
        f"M {x+r:.2f} {y:.2f} H {x+w-r:.2f} Q {x+w:.2f} {y:.2f} {x+w:.2f} {y+r:.2f} "
        f"V {y+h-r:.2f} Q {x+w:.2f} {y+h:.2f} {x+w-r:.2f} {y+h:.2f} "
        f"H {x+r:.2f} Q {x:.2f} {y+h:.2f} {x:.2f} {y+h-r:.2f} "
        f"V {y+r:.2f} Q {x:.2f} {y:.2f} {x+r:.2f} {y:.2f} Z"
    )


def arc_path(cx: float, cy: float, r: float, start_deg: float, end_deg: float, sweep: int = 1) -> str:
    x1, y1 = point(cx, cy, r, start_deg)
    x2, y2 = point(cx, cy, r, end_deg)
    delta = abs(end_deg - start_deg) % 360
    large_arc = 1 if delta > 180 else 0
    return f"M {x1:.2f} {y1:.2f} A {r:.2f} {r:.2f} 0 {large_arc} {sweep} {x2:.2f} {y2:.2f}"


def callout(
    s: Svg,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    text: str,
    fill: str = "#334155",
    anchor: str = "start",
) -> None:
    s.line(x1, y1, x2, y2, fill, 1.8)
    s.circle(x1, y1, 3.2, fill, fill, 1)
    s.text(x2 + (8 if anchor == "start" else -8), y2 + 5, text, 13, fill, "700", anchor)


def main() -> None:
    s = Svg()
    width = 1600
    height = 1000

    # Preview values mirrored from generate_remote_case_stl.py.
    base_w = 44.0
    base_l = 70.0
    remote_od = 48.96
    visible_d = 44.16
    magnet_w = 10.0
    magnet_l = 60.0
    battery_w = 25.5
    battery_l = 37.0

    s.text(54, 66, "SG98 Remote Back-Cover Case Preview", 28, "#111827", "700")
    s.text(54, 96, "Narrow lower case sits under the remote, leaving the two control sides exposed.", 14, "#5b6472")

    # Top assembly view: remote mounted on the base.
    scale = 6.4
    bx = 270
    by = 170
    bw = base_w * scale
    bl = base_l * scale
    rcx = bx + bw / 2
    rcy = by + bl / 2
    rr = remote_od * scale / 2
    vr = visible_d * scale / 2

    s.text(54, 155, "Top Assembly", 20, "#111827", "700")
    s.path(rounded_rect_path(bx + 10, by + 12, bw, bl, 42), "#000000", "#000000", 0)
    s.items[-1] = s.items[-1].replace('fill="#000000"', 'fill="#000000" opacity="0.08"')
    s.path(rounded_rect_path(bx, by, bw, bl, 42), "url(#caseGrad)", "#334155", 2.2)
    s.text(bx + 26, by + 36, "lower back-cover case", 13, "#475569", "700")

    socket_r = (remote_od / 2 + 0.7) * scale
    s.circle(rcx, rcy, socket_r, "#dbe4ee", "#64748b", 1.5)
    s.items[-1] = s.items[-1].replace('fill="#dbe4ee"', 'fill="#dbe4ee" opacity="0.72"')
    s.text(rcx, rcy - socket_r - 14, "open-sided round socket", 13, "#475569", "700", "middle")
    s.circle(rcx, rcy, rr + 14, "#e2e8f0", "#94a3b8", 1.4)
    s.items[-1] = s.items[-1].replace('fill="#e2e8f0"', 'fill="#e2e8f0" opacity="0.42"')
    s.circle(rcx, rcy, rr, "url(#boardGrad)", "#365314", 1.6)
    s.circle(rcx, rcy, vr, "url(#glassGrad)", "#020617", 2.2)
    s.text(rcx, rcy + 5, "remote recessed into case", 15, "#f8fafc", "700", "middle")

    callout(s, rcx - 18, rcy - socket_r + 36, bx - 34, by + 82, "round socket wall", "#475569", "end")
    callout(s, rcx + rr + 8, rcy + 14, bx + bw + 108, rcy + 12, "remote side overhang", "#7c3aed", "start")
    callout(s, rcx + 8, rcy + rr + 18, rcx + 92, by + bl + 36, "control side exposed", "#2563eb", "start")

    s.rect(rcx - 70, by - 23, 140, 34, "#fee2e2", "#dc2626", 2, 14)
    s.items[-1] = s.items[-1].replace('fill="#fee2e2"', 'fill="#fee2e2" opacity="0.78"')
    s.circle(rcx, by - 6, 10, "#fff7ed", "#dc2626", 2)
    s.text(rcx, by - 34, "recessed pressure socket", 13, "#dc2626", "700", "middle")
    s.rect(rcx - 44, by + bl + 10, 88, 18, "#dbeafe", "#2563eb", 2, 6)
    s.text(rcx, by + bl + 48, "USB-C side stays exposed", 13, "#2563eb", "700", "middle")

    for label, yoff in [("PWR", -52), ("BOOT", 52)]:
        s.circle(bx + bw + 34, rcy + yoff, 7, "#7c3aed", "#7c3aed", 1)
        s.text(bx + bw + 92, rcy + yoff + 5, f"{label} exposed", 13, "#7c3aed", "700", "middle")

    # Inner bottom lid view: this is where the magnet rails are visible.
    lx = 980
    ly = 172
    lw = base_w * scale
    ll = base_l * scale
    s.text(lx, 155, "Inner Bottom Lid", 20, "#111827", "700")
    s.path(rounded_rect_path(lx, ly, lw, ll, 42), "#edf2f7", "#334155", 2.2)
    s.path(rounded_rect_path(lx + 22, ly + 22, lw - 44, ll - 44, 30), "#ffffff", "#94a3b8", 1.2)
    s.items[-1] = s.items[-1].replace('fill="#ffffff"', 'fill="#ffffff" opacity="0.58"')

    mx = lx + (lw - magnet_w * scale) / 2
    my = ly + (ll - magnet_l * scale) / 2
    rail = 9
    s.rect(mx, my, magnet_w * scale, magnet_l * scale, "#c4b5fd", "#7c3aed", 1.4, 6)
    s.items[-1] = s.items[-1].replace('fill="#c4b5fd"', 'fill="#c4b5fd" opacity="0.68"')
    s.rect(mx - rail, my - rail, rail, magnet_l * scale + 2 * rail, "#ede9fe", "#7c3aed", 1.4, 3)
    s.rect(mx + magnet_w * scale, my - rail, rail, magnet_l * scale + 2 * rail, "#ede9fe", "#7c3aed", 1.4, 3)
    s.rect(mx - rail, my - rail, magnet_w * scale + 2 * rail, rail, "#ede9fe", "#7c3aed", 1.4, 3)
    s.rect(mx - rail, my + magnet_l * scale, magnet_w * scale + 2 * rail, rail, "#ede9fe", "#7c3aed", 1.4, 3)
    s.text(lx + lw / 2, my - 24, "recessed pocket plus retaining rails", 13, "#7c3aed", "700", "middle")

    for hy in [ly + ll / 2 - 18 * scale, ly + ll / 2 + 18 * scale]:
        s.rect(lx - 12, hy - 23, 18, 46, "#cbd5e1", "#475569", 1.5, 4)
        s.rect(lx + lw - 6, hy - 23, 18, 46, "#cbd5e1", "#475569", 1.5, 4)
    s.text(lx + lw / 2, ly + ll + 28, "4 snap tabs click into matching shell slots", 13, "#475569", "700", "middle")

    batt_x = lx + 44
    batt_y = ly + 160
    s.rect(batt_x, batt_y, battery_w * scale, battery_l * scale, "url(#batteryGrad)", "#020617", 1.4, 10)
    s.text(batt_x + battery_w * scale / 2, batt_y + battery_l * scale / 2 + 5, "battery", 13, "#e5e7eb", "700", "middle")

    # Side section.
    sx = 90
    sy = 860
    s.text(sx, sy - 190, "Section A-A", 20, "#111827", "700")
    s.text(sx, sy - 164, "The remote drops into the open-sided round upper socket; wiring, battery, and magnet are enclosed below it.", 13, "#64748b")
    s.rect(sx + 250, sy - 150, 390, 112, "url(#boardGrad)", "#365314", 1.5, 8)
    s.rect(sx + 250, sy - 166, 390, 20, "url(#glassGrad)", "#020617", 1.4, 6)
    s.text(sx + 445, sy - 176, "remote", 13, "#111827", "700", "middle")
    s.rect(sx + 225, sy - 104, 440, 92, "#dbe4ee", "#334155", 2, 10)
    s.rect(sx + 260, sy - 112, 370, 20, "#e2e8f0", "#64748b", 1.5, 7)
    s.text(sx + 445, sy - 54, "deep upper socket and wiring cavity", 13, "#334155", "700", "middle")
    s.rect(sx + 230, sy - 60, 200, 34, "url(#batteryGrad)", "#020617", 1.2, 7)
    s.text(sx + 330, sy - 38, "battery", 12, "#e5e7eb", "700", "middle")
    s.rect(sx + 225, sy - 18, 440, 36, "#edf2f7", "#334155", 1.8, 8)
    s.rect(sx + 315, sy - 10, 280, 16, "#c4b5fd", "#7c3aed", 1.2, 4)
    s.items[-1] = s.items[-1].replace('fill="#c4b5fd"', 'fill="#c4b5fd" opacity="0.68"')
    s.rect(sx + 305, sy - 14, 10, 24, "#ede9fe", "#7c3aed", 1.2, 3)
    s.rect(sx + 595, sy - 14, 10, 24, "#ede9fe", "#7c3aed", 1.2, 3)
    s.text(sx + 455, sy + 34, "magnet is recessed into snap-in bottom lid", 13, "#7c3aed", "700", "middle")

    s.rect(54, 928, 1492, 46, "#ffffff", "#d1d5db", 1.2, 12)
    s.text(78, 957, "Back-cover print set: 44 mm narrow case, open-sided round remote socket, recessed M5 pressure connector bay, snap-in magnet lid.", 14, "#111827", "600")

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
                "<defs>",
                '<linearGradient id="caseGrad" x1="0" x2="1" y1="0" y2="1">',
                '<stop offset="0%" stop-color="#f8fafc" />',
                '<stop offset="100%" stop-color="#cbd5e1" />',
                "</linearGradient>",
                '<linearGradient id="batteryGrad" x1="0" x2="1" y1="0" y2="1">',
                '<stop offset="0%" stop-color="#1f2937" />',
                '<stop offset="100%" stop-color="#030712" />',
                "</linearGradient>",
                '<linearGradient id="boardGrad" x1="0" x2="1" y1="0" y2="1">',
                '<stop offset="0%" stop-color="#bef264" />',
                '<stop offset="100%" stop-color="#4d7c0f" />',
                "</linearGradient>",
                '<linearGradient id="glassGrad" x1="0" x2="1" y1="0" y2="1">',
                '<stop offset="0%" stop-color="#1f2937" />',
                '<stop offset="100%" stop-color="#020617" />',
                "</linearGradient>",
                "</defs>",
                f'<rect width="{width}" height="{height}" fill="#f6f7f9" />',
                *s.items,
            ]
        ),
    )
    OUT.write_text(svg + "\n", encoding="utf-8")
    print(OUT)


if __name__ == "__main__":
    main()
