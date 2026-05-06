#!/usr/bin/env python3
"""Generate the SG98 burr-gap digital indicator wiring PDF."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "Documentation" / "sg98-burr-gap-indicator-wiring.pdf"


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
        approx = len(text) * size * 0.245
        self.text(x - approx, y, text, size, font)


def write_pdf(pdf: Pdf, path: Path) -> None:
    stream = "\n".join(pdf.commands).encode("utf-8")
    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 4 0 R /F2 5 0 R /F3 6 0 R >> >> "
            b"/Contents 7 0 R >>"
        ),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
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


def block(pdf: Pdf, x: float, y: float, w: float, h: float, title: str, subtitle: str, fill: tuple[float, float, float]) -> None:
    pdf.color(*fill, stroke=True, fill=True)
    pdf.line_width(1.1)
    pdf.rect(x, y, w, h, fill=True, stroke=True)
    pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
    pdf.text(x + 14, y + h - 28, title, 13, "F2")
    pdf.text(x + 14, y + h - 45, subtitle, 8.5)


def draw_wire(
    pdf: Pdf,
    points: list[tuple[float, float]],
    rgb: tuple[float, float, float],
    label: str,
    label_x: float,
    label_y: float,
) -> None:
    pdf.color(*rgb, stroke=True, fill=True)
    pdf.line_width(4.0)
    pdf.polyline(points)
    pdf.circle(points[0][0], points[0][1], 3.7, fill=True, stroke=False)
    pdf.circle(points[-1][0], points[-1][1], 3.7, fill=True, stroke=False)
    if label:
        pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
        pdf.text(label_x, label_y, label, 8.3, "F2")


def main() -> None:
    pdf = Pdf()

    pdf.color(1.0, 1.0, 1.0, fill=True, stroke=False)
    pdf.rect(0, 0, 612, 792, fill=True, stroke=False)

    pdf.color(0.08, 0.09, 0.11, fill=True, stroke=False)
    pdf.text(42, 746, "SG98 Burr-Gap Digital Indicator Wiring", 22, "F2")
    pdf.text(42, 724, "GUIRUIMOON FS PFC 12mm indicator -> 4-channel level shifter -> SG98 controller EX_I2C header", 10.5)

    pdf.color(0.94, 0.96, 0.98, fill=True, stroke=True)
    pdf.line_width(0.8)
    pdf.rect(40, 668, 532, 40, fill=True, stroke=True)
    pdf.color(0.12, 0.13, 0.15, fill=True, stroke=False)
    pdf.text(54, 691, "Assumption: the indicator exposes the common 4-wire clock/data port: V+, GND, CLK, DATA.", 9.2, "F2")
    pdf.text(54, 676, "Keep the indicator battery installed unless you confirm the gauge accepts external 3.3V power.", 9.2)

    # Main diagram with the level-shifter pin order matching the photographed board.
    block(pdf, 44, 436, 168, 190, "Digital Indicator", "Low-voltage gauge side", (0.98, 0.94, 0.86))
    block(pdf, 230, 392, 152, 266, "Level Shifter", "Photo view: text on right", (0.91, 0.96, 1.0))
    block(pdf, 400, 436, 168, 190, "SG98 Controller", "EX_I2C header as GPIO", (0.91, 0.97, 0.92))

    row_y = {
        "power": 574,
        "gnd": 540,
        "clk": 506,
        "data": 472,
    }
    colors = {
        "power": (0.91, 0.48, 0.02),
        "hv": (0.86, 0.12, 0.12),
        "gnd": (0.08, 0.09, 0.11),
        "clk": (0.05, 0.36, 0.86),
        "data": (0.00, 0.50, 0.28),
        "oe": (0.48, 0.26, 0.68),
        "unused": (0.52, 0.56, 0.62),
    }

    ind_dot_x = 202
    ctl_dot_x = 410
    board_x = 242
    board_y = 396
    board_w = 128
    board_h = 204
    pin_x = [board_x + 12 + i * 21 for i in range(6)]
    top_y = board_y + board_h - 30
    bottom_y = board_y + 28

    indicator_rows = [
        ("V+ / battery +", "power"),
        ("GND", "gnd"),
        ("CLK", "clk"),
        ("DATA", "data"),
    ]
    for label, key in indicator_rows:
        y = row_y[key]
        pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
        pdf.text(62, y - 4, label, 9.4, "F2")
        pdf.color(*colors[key], fill=True, stroke=False)
        pdf.circle(ind_dot_x, y, 4.2, fill=True, stroke=False)

    controller_rows = [
        ("3V3", "hv", "power"),
        ("GND", "gnd", "gnd"),
        ("GPIO9 / CLK in", "clk", "clk"),
        ("GPIO8 / DATA in", "data", "data"),
    ]
    for label, color_key, y_key in controller_rows:
        y = row_y[y_key]
        pdf.color(*colors[color_key], fill=True, stroke=False)
        pdf.circle(ctl_dot_x, y, 4.2, fill=True, stroke=False)
        pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
        pdf.text(424, y - 4, label, 9.4, "F2")

    pdf.text(424, 452, "Header order:", 8.2, "F2")
    pdf.text(424, 438, "1 3V3  2 GND  3 GPIO9  4 GPIO8", 7.8, "F3")

    # Photo board outline and pin order. Upper row is the controller/HV side;
    # lower row is the indicator/LV side.
    pdf.color(0.05, 0.055, 0.06, fill=True, stroke=True)
    pdf.rect(board_x, board_y, board_w, board_h, fill=True, stroke=True)
    pdf.color(1.0, 1.0, 1.0, fill=True, stroke=False)
    pdf.centered_text(board_x + board_w / 2, board_y + board_h - 15, "top row to SG98", 6.8, "F2")
    pdf.centered_text(board_x + board_w / 2, board_y + 12, "bottom row to indicator", 6.8, "F2")
    pdf.color(0.78, 0.82, 0.88, stroke=True)
    pdf.line_width(0.8)
    pdf.line(pin_x[1], bottom_y + 8, pin_x[1], top_y - 8)
    pdf.line(pin_x[2], bottom_y + 8, pin_x[2], top_y - 8)

    bottom_pins = {
        "lv": (pin_x[0], bottom_y),
        "a1": (pin_x[1], bottom_y),
        "a2": (pin_x[2], bottom_y),
        "a3": (pin_x[3], bottom_y),
        "a4": (pin_x[4], bottom_y),
        "oe": (pin_x[5], bottom_y),
    }
    top_pins = {
        "hv": (pin_x[0], top_y),
        "b1": (pin_x[1], top_y),
        "b2": (pin_x[2], top_y),
        "b3": (pin_x[3], top_y),
        "b4": (pin_x[4], top_y),
        "gnd": (pin_x[5], top_y),
    }

    # Signal wires: details are in the table below.
    draw_wire(
        pdf,
        [(ind_dot_x, row_y["power"]), (222, row_y["power"]), (222, 438), bottom_pins["lv"]],
        colors["power"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [(ind_dot_x, row_y["gnd"]), (230, row_y["gnd"]), (230, top_y), top_pins["gnd"]],
        colors["gnd"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [(ind_dot_x, row_y["clk"]), (214, row_y["clk"]), (214, 422), bottom_pins["a1"]],
        colors["clk"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [(ind_dot_x, row_y["data"]), (210, row_y["data"]), (210, 414), bottom_pins["a2"]],
        colors["data"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [top_pins["hv"], (386, top_y), (386, row_y["power"]), (ctl_dot_x, row_y["power"])],
        colors["hv"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [top_pins["gnd"], (390, top_y), (390, row_y["gnd"]), (ctl_dot_x, row_y["gnd"])],
        colors["gnd"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [top_pins["b1"], (394, top_y), (394, row_y["clk"]), (ctl_dot_x, row_y["clk"])],
        colors["clk"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [top_pins["b2"], (398, top_y), (398, row_y["data"]), (ctl_dot_x, row_y["data"])],
        colors["data"],
        "",
        0,
        0,
    )
    draw_wire(
        pdf,
        [bottom_pins["lv"], (pin_x[0], 384), (pin_x[5], 384), bottom_pins["oe"]],
        colors["oe"],
        "",
        0,
        0,
    )

    top_labels = [("HV", "hv"), ("B1", "clk"), ("B2", "data"), ("B3", "unused"), ("B4", "unused"), ("GND", "gnd")]
    bottom_labels = [("LV", "power"), ("A1", "clk"), ("A2", "data"), ("A3", "unused"), ("A4", "unused"), ("OE", "oe")]
    pin_text_colors = {
        "power": (1.0, 0.66, 0.16),
        "hv": (1.0, 0.36, 0.36),
        "gnd": (1.0, 1.0, 1.0),
        "clk": (0.36, 0.62, 1.0),
        "data": (0.26, 0.78, 0.48),
        "oe": (0.78, 0.56, 1.0),
        "unused": (0.68, 0.71, 0.78),
    }
    for idx, (label, key) in enumerate(top_labels):
        x = pin_x[idx]
        pdf.color(0.84, 0.84, 0.80, fill=True, stroke=False)
        pdf.circle(x, top_y, 5.6, fill=True, stroke=False)
        pdf.color(0.05, 0.055, 0.06, fill=True, stroke=False)
        pdf.circle(x, top_y, 3.0, fill=True, stroke=False)
        pdf.color(*pin_text_colors[key], fill=True, stroke=False)
        pdf.centered_text(x, top_y - 18, label, 7.3, "F3")
    for idx, (label, key) in enumerate(bottom_labels):
        x = pin_x[idx]
        pdf.color(0.84, 0.84, 0.80, fill=True, stroke=False)
        pdf.circle(x, bottom_y, 5.6, fill=True, stroke=False)
        pdf.color(0.05, 0.055, 0.06, fill=True, stroke=False)
        pdf.circle(x, bottom_y, 3.0, fill=True, stroke=False)
        pdf.color(*pin_text_colors[key], fill=True, stroke=False)
        pdf.centered_text(x, bottom_y + 11, label, 7.3, "F3")

    pdf.color(0.40, 0.44, 0.50, fill=True, stroke=False)
    pdf.centered_text(306, 526, "A1/B1 = CLK", 8.0, "F2")
    pdf.centered_text(306, 512, "A2/B2 = DATA", 8.0, "F2")
    pdf.centered_text(306, 388, "OE jumper to LV", 7.4, "F2")

    # Connection table.
    table_x = 48
    table_y = 196
    pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
    pdf.text(table_x, table_y + 136, "Connection Table", 14, "F2")
    pdf.color(0.95, 0.96, 0.98, fill=True, stroke=True)
    pdf.rect(table_x, table_y, 516, 124, fill=True, stroke=True)
    pdf.color(0.20, 0.22, 0.26, fill=True, stroke=False)
    pdf.text(table_x + 14, table_y + 102, "Indicator", 9, "F2")
    pdf.text(table_x + 158, table_y + 102, "Level shifter", 9, "F2")
    pdf.text(table_x + 312, table_y + 102, "Controller", 9, "F2")
    pdf.color(0.70, 0.73, 0.78, stroke=True)
    pdf.line(table_x, table_y + 94, table_x + 516, table_y + 94)
    rows = [
        ("V+ / battery +", "bottom LV + OE jumper", "no direct controller pin"),
        ("GND", "top GND, shared", "EX_I2C pin 2 / GND"),
        ("CLK", "bottom A1 -> top B1", "EX_I2C pin 3 / GPIO9"),
        ("DATA", "bottom A2 -> top B2", "EX_I2C pin 4 / GPIO8"),
        ("none", "top HV", "EX_I2C pin 1 / 3V3"),
    ]
    for idx, row in enumerate(rows):
        y = table_y + 74 - idx * 18
        pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
        pdf.text(table_x + 14, y, row[0], 8.7)
        pdf.text(table_x + 158, y, row[1], 8.7)
        pdf.text(table_x + 312, y, row[2], 8.7)

    # Notes.
    notes_x = 48
    notes_y = 66
    pdf.color(1.0, 0.97, 0.90, fill=True, stroke=True)
    pdf.rect(notes_x, notes_y, 516, 94, fill=True, stroke=True)
    pdf.color(0.11, 0.12, 0.14, fill=True, stroke=False)
    pdf.text(notes_x + 14, notes_y + 70, "Bring-up notes", 12, "F2")
    pdf.text(notes_x + 14, notes_y + 52, "1. Confirm GND and V+ at the gauge battery/data connector before wiring.", 8.6)
    pdf.text(notes_x + 14, notes_y + 38, "2. Probe the remaining two pins while moving the plunger; assign CLK and DATA by waveform.", 8.6)
    pdf.text(notes_x + 14, notes_y + 24, "3. Photo board pin order: top HV B1 B2 B3 B4 GND; bottom LV A1 A2 A3 A4 OE.", 8.6)
    pdf.text(notes_x + 14, notes_y + 10, "4. Jumper OE to LV; tie SG98 GND, shifter top GND, and gauge GND together.", 8.6, "F2")

    write_pdf(pdf, OUT)
    print(OUT)


if __name__ == "__main__":
    main()
