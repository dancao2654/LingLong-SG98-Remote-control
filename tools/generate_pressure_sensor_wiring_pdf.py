#!/usr/bin/env python3
"""Generate the remote pressure sensor wiring PDF."""

from __future__ import annotations

import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "Documentation" / "remote-pressure-sensor-wiring.pdf"


def esc(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


class Pdf:
    def __init__(self) -> None:
        self.commands: list[str] = []

    def raw(self, command: str) -> None:
        self.commands.append(command)

    def color(self, r: float, g: float, b: float, stroke: bool = True, fill: bool = False) -> None:
        if stroke:
            self.raw(f"{r:.3f} {g:.3f} {b:.3f} RG")
        if fill:
            self.raw(f"{r:.3f} {g:.3f} {b:.3f} rg")

    def line_width(self, width: float) -> None:
        self.raw(f"{width:.2f} w")

    def rect(self, x: float, y: float, w: float, h: float, fill: bool = False, stroke: bool = True) -> None:
        op = "B" if fill and stroke else "f" if fill else "S"
        self.raw(f"{x:.2f} {y:.2f} {w:.2f} {h:.2f} re {op}")

    def line(self, x1: float, y1: float, x2: float, y2: float) -> None:
        self.raw(f"{x1:.2f} {y1:.2f} m {x2:.2f} {y2:.2f} l S")

    def polyline(self, points: list[tuple[float, float]]) -> None:
        if len(points) < 2:
            return
        first = points[0]
        parts = [f"{first[0]:.2f} {first[1]:.2f} m"]
        parts.extend(f"{x:.2f} {y:.2f} l" for x, y in points[1:])
        parts.append("S")
        self.raw(" ".join(parts))

    def circle(self, x: float, y: float, r: float, fill: bool = False, stroke: bool = True) -> None:
        k = 0.5522847498
        c = r * k
        op = "B" if fill and stroke else "f" if fill else "S"
        self.raw(
            " ".join(
                [
                    f"{x + r:.2f} {y:.2f} m",
                    f"{x + r:.2f} {y + c:.2f} {x + c:.2f} {y + r:.2f} {x:.2f} {y + r:.2f} c",
                    f"{x - c:.2f} {y + r:.2f} {x - r:.2f} {y + c:.2f} {x - r:.2f} {y:.2f} c",
                    f"{x - r:.2f} {y - c:.2f} {x - c:.2f} {y - r:.2f} {x:.2f} {y - r:.2f} c",
                    f"{x + c:.2f} {y - r:.2f} {x + r:.2f} {y - c:.2f} {x + r:.2f} {y:.2f} c",
                    op,
                ]
            )
        )

    def text(self, x: float, y: float, text: str, size: float = 10, font: str = "F1") -> None:
        self.raw(f"BT /{font} {size:.1f} Tf {x:.2f} {y:.2f} Td ({esc(text)}) Tj ET")

    def centered_text(self, x: float, y: float, text: str, size: float = 10, font: str = "F1") -> None:
        approx = len(text) * size * 0.25
        self.text(x - approx, y, text, size, font)


def write_pdf(pdf: Pdf, path: Path) -> None:
    stream = "\n".join(pdf.commands).encode("utf-8")
    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> "
            b"/Contents 6 0 R >>"
        ),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
        b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"\nendstream",
    ]
    chunks = [b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"]
    offsets = [0]
    for idx, obj in enumerate(objects, start=1):
        offsets.append(sum(len(chunk) for chunk in chunks))
        chunks.append(f"{idx} 0 obj\n".encode("ascii"))
        chunks.append(obj)
        chunks.append(b"\nendobj\n")
    xref_offset = sum(len(chunk) for chunk in chunks)
    chunks.append(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    chunks.append(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        chunks.append(f"{offset:010d} 00000 n \n".encode("ascii"))
    chunks.append(
        (
            f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
            f"startxref\n{xref_offset}\n%%EOF\n"
        ).encode("ascii")
    )
    path.write_bytes(b"".join(chunks))


def main() -> None:
    pdf = Pdf()
    page_w = 612

    pdf.color(1.0, 1.0, 1.0, fill=True, stroke=False)
    pdf.rect(0, 0, 612, 792, fill=True, stroke=False)

    pdf.color(0.08, 0.09, 0.11, stroke=True, fill=True)
    pdf.text(46, 742, "Remote Pressure Sensor Wiring", 23, "F2")
    pdf.text(46, 720, "Waveshare ESP32-S3 Touch AMOLED 1.75 - H2 8-pin header, back side view", 11)

    pdf.color(0.93, 0.94, 0.95, fill=True, stroke=False)
    pdf.rect(42, 662, 528, 34, fill=True, stroke=False)
    pdf.color(0.42, 0.45, 0.50, stroke=True)
    pdf.line_width(0.8)
    pdf.rect(42, 662, 528, 34, fill=False, stroke=True)
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(56, 674, "Orientation: back of remote facing you, USB-C connector on the right, 8-pin header along the bottom.", 10)

    # Remote board outline and USB-C side cue.
    pdf.color(0.96, 0.97, 0.98, fill=True, stroke=True)
    pdf.line_width(1.0)
    pdf.rect(75, 276, 462, 214, fill=True, stroke=True)
    pdf.color(0.74, 0.77, 0.82, fill=True, stroke=True)
    pdf.rect(520, 350, 26, 72, fill=True, stroke=True)
    pdf.color(0.25, 0.27, 0.30, fill=True, stroke=False)
    pdf.text(516, 429, "USB-C", 9, "F2")
    pdf.text(250, 470, "REMOTE BACK", 13, "F2")

    labels = ["IO18", "IO17", "IO16", "RXD", "TXD", "3V3", "GND", "VBUS"]
    header_x = 128
    header_y = 316
    pin_w = 42
    gap = 5
    pin_h = 42
    centers: list[tuple[float, float]] = []

    for i, label in enumerate(labels):
        x = header_x + i * (pin_w + gap)
        centers.append((x + pin_w / 2, header_y + pin_h / 2))
        if i == 1:
            pdf.color(0.84, 0.96, 0.89, fill=True, stroke=True)
        elif i == 6:
            pdf.color(0.90, 0.92, 0.94, fill=True, stroke=True)
        elif i == 7:
            pdf.color(1.00, 0.90, 0.90, fill=True, stroke=True)
        else:
            pdf.color(0.88, 0.91, 0.95, fill=True, stroke=True)
        pdf.rect(x, header_y, pin_w, pin_h, fill=True, stroke=True)
        pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
        pdf.centered_text(x + pin_w / 2, header_y + 25, label, 9, "F2")
        pdf.centered_text(x + pin_w / 2, header_y - 16, str(i + 1), 10, "F2")

    pdf.text(header_x, header_y - 38, "Left to right pin numbers on H2 header", 10)

    # Sensor block.
    sensor_x = 56
    sensor_y = 520
    sensor_w = 220
    sensor_h = 124
    pdf.color(0.98, 0.95, 0.88, fill=True, stroke=True)
    pdf.rect(sensor_x, sensor_y, sensor_w, sensor_h, fill=True, stroke=True)
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(sensor_x + 16, sensor_y + 96, "Pressure Sensor Cable", 13, "F2")
    pdf.text(sensor_x + 16, sensor_y + 80, "0-2 MPa sensor, 0.4-2.4V output", 9)

    terminal_x = sensor_x + sensor_w - 12
    sensor_terms = [
        ("SUP / +5V IN", "sensor power", (0.82, 0.10, 0.10), sensor_y + 58),
        ("OUT / SIGNAL", "pressure voltage", (0.00, 0.49, 0.30), sensor_y + 36),
        ("GND", "common ground", (0.12, 0.13, 0.15), sensor_y + 14),
    ]
    for name, detail, color, term_y in sensor_terms:
        pdf.color(*color, stroke=True, fill=True)
        pdf.circle(terminal_x, term_y, 4.2, fill=True, stroke=False)
        pdf.line(terminal_x - 16, term_y, terminal_x, term_y)
        pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
        pdf.text(sensor_x + 16, term_y + 3, name, 10, "F2")
        pdf.text(sensor_x + 112, term_y + 3, detail, 8)

    wire_specs: list[tuple[str, list[tuple[float, float]], tuple[float, float], tuple[float, float], tuple[float, float]]] = [
        (
            "+5V -> pin 8",
            [(terminal_x, sensor_y + 58), (478, sensor_y + 58), (478, 500), (centers[7][0], 500), (centers[7][0], header_y + pin_h + 4)],
            (0.82, 0.10, 0.10),
            (382, sensor_y + 64),
            (terminal_x, sensor_y + 58),
        ),
        (
            "OUT -> pin 2",
            [(terminal_x, sensor_y + 36), (286, sensor_y + 36), (286, 430), (centers[1][0], 430), (centers[1][0], header_y + pin_h + 4)],
            (0.00, 0.49, 0.30),
            (292, 438),
            (terminal_x, sensor_y + 36),
        ),
        (
            "GND -> pin 7",
            [(terminal_x, sensor_y + 14), (430, sensor_y + 14), (430, 410), (centers[6][0], 410), (centers[6][0], header_y + pin_h + 4)],
            (0.12, 0.13, 0.15),
            (436, 418),
            (terminal_x, sensor_y + 14),
        ),
    ]

    for label, points, color, label_pos, start in wire_specs:
        end = points[-1]
        pdf.color(*color, stroke=True, fill=True)
        pdf.line_width(4.0)
        pdf.polyline(points)
        pdf.circle(start[0], start[1], 3.8, fill=True, stroke=False)
        pdf.circle(end[0], end[1], 3.8, fill=True, stroke=False)
        pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
        pdf.text(label_pos[0], label_pos[1], label, 8, "F2")

    # Optional external 5V charging input for the battery connector.
    charge_x = 324
    charge_y = 548
    charge_w = 226
    charge_h = 92
    pdf.color(0.91, 0.96, 1.00, fill=True, stroke=True)
    pdf.rect(charge_x, charge_y, charge_w, charge_h, fill=True, stroke=True)
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(charge_x + 14, charge_y + 70, "Battery Charging Input", 12, "F2")
    pdf.text(charge_x + 14, charge_y + 52, "Use a regulated 5V source:", 9)
    pdf.color(0.82, 0.10, 0.10, fill=True, stroke=False)
    pdf.text(charge_x + 22, charge_y + 36, "+5V -> H2 pin 8 / VBUS", 9, "F2")
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(charge_x + 22, charge_y + 22, "GND -> H2 pin 7 / GND", 9, "F2")
    pdf.text(charge_x + 14, charge_y + 8, "MX1.25 battery stays on battery connector.", 8)

    # Connection table.
    table_x = 62
    table_y = 132
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(table_x, table_y + 110, "Connections", 14, "F2")
    pdf.color(0.93, 0.95, 0.97, fill=True, stroke=True)
    pdf.rect(table_x, table_y, 488, 100, fill=True, stroke=True)
    rows = [
        ("Sensor SUP / +5V", "H2 pin 8", "VBUS / 5V power"),
        ("Sensor OUT", "H2 pin 2", "IO17 ADC input, firmware pressure signal"),
        ("Sensor GND", "H2 pin 7", "GND, common with remote"),
        ("External 5V charger +", "H2 pin 8", "VBUS / PMIC charge input"),
        ("External 5V charger -", "H2 pin 7", "GND, common with battery charger"),
    ]
    for idx, row in enumerate(rows):
        y = table_y + 78 - idx * 18
        if idx:
            pdf.color(0.74, 0.77, 0.82, stroke=True)
            pdf.line(table_x, y + 13, table_x + 488, y + 13)
        pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
        pdf.text(table_x + 14, y, row[0], 9, "F2")
        pdf.text(table_x + 178, y, row[1], 9)
        pdf.text(table_x + 292, y, row[2], 9)

    # Safety notes.
    pdf.color(0.99, 0.93, 0.80, fill=True, stroke=True)
    pdf.rect(62, 70, 488, 54, fill=True, stroke=True)
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(76, 104, "Notes", 11, "F2")
    pdf.text(76, 90, "Do not feed 5V into IO17 or any GPIO. Sensor OUT must stay in the 0.4-2.4V range.", 8)
    pdf.text(76, 77, "To charge the MX1.25 battery, feed regulated 5V only into H2 pin 8 and GND into H2 pin 7.", 8)
    pdf.text(76, 64, "Do not connect the LiPo battery to H2. It must stay on the MX1.25 battery connector.", 8)

    pdf.color(0.42, 0.45, 0.50, fill=True, stroke=False)
    pdf.text(46, 42, "Generated from firmware setting: kPressureSensorAdcPin = GPIO17.", 8)
    pdf.text(46, 30, "Source pinout: Waveshare ESP32-S3-Touch-AMOLED-1.75 documentation and schematic.", 8)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    write_pdf(pdf, OUT)


if __name__ == "__main__":
    main()
