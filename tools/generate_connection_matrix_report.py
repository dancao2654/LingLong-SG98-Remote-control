#!/usr/bin/env python3
"""Generate the remote/controller connection matrix PDF."""

from __future__ import annotations

from pathlib import Path
from textwrap import wrap


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "Documentation" / "remote-controller-connection-matrix-report.pdf"


def esc(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


class Pdf:
    def __init__(self) -> None:
        self.pages: list[list[str]] = [[]]

    @property
    def commands(self) -> list[str]:
        return self.pages[-1]

    def page(self) -> None:
        self.pages.append([])

    def raw(self, command: str) -> None:
        self.commands.append(command)

    def color(self, r: float, g: float, b: float, *, stroke: bool = True, fill: bool = False) -> None:
        if stroke:
            self.raw(f"{r:.3f} {g:.3f} {b:.3f} RG")
        if fill:
            self.raw(f"{r:.3f} {g:.3f} {b:.3f} rg")

    def rect(self, x: float, y: float, w: float, h: float, *, fill: bool = False, stroke: bool = True) -> None:
        op = "B" if fill and stroke else "f" if fill else "S"
        self.raw(f"{x:.2f} {y:.2f} {w:.2f} {h:.2f} re {op}")

    def text(
        self,
        x: float,
        y: float,
        text: str,
        size: float = 8,
        font: str = "F1",
        fill_color: tuple[float, float, float] = (0.10, 0.12, 0.15),
    ) -> None:
        self.color(*fill_color, stroke=False, fill=True)
        self.raw(f"BT /{font} {size:.1f} Tf {x:.2f} {y:.2f} Td ({esc(text)}) Tj ET")


def write_pdf(pdf: Pdf, path: Path) -> None:
    streams = ["\n".join(page).encode("utf-8") for page in pdf.pages]
    page_ids = [3 + i for i in range(len(streams))]
    font1_id = 3 + len(streams)
    font2_id = font1_id + 1
    content_ids = [font2_id + 1 + i for i in range(len(streams))]
    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        (
            b"<< /Type /Pages /Kids ["
            + b" ".join(f"{pid} 0 R".encode("ascii") for pid in page_ids)
            + b"] /Count "
            + str(len(streams)).encode("ascii")
            + b" >>"
        ),
    ]
    for content_id in content_ids:
        objects.append(
            (
                b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] "
                b"/Resources << /Font << /F1 "
                + str(font1_id).encode("ascii")
                + b" 0 R /F2 "
                + str(font2_id).encode("ascii")
                + b" 0 R >> >> /Contents "
                + str(content_id).encode("ascii")
                + b" 0 R >>"
            )
        )
    objects.extend(
        [
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
        ]
    )
    for stream in streams:
        objects.append(b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"\nendstream")

    chunks = [b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"]
    offsets = [0]
    for idx, obj in enumerate(objects, start=1):
        offsets.append(sum(len(chunk) for chunk in chunks))
        chunks.append(f"{idx} 0 obj\n".encode("ascii"))
        chunks.append(obj)
        chunks.append(b"\nendobj\n")
    xref_offset = sum(len(chunk) for chunk in chunks)
    chunks.append(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    chunks.append(b"0000000000 65535 f\n")
    for offset in offsets[1:]:
        chunks.append(f"{offset:010d} 00000 n\n".encode("ascii"))
    chunks.append(
        (
            f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
            f"startxref\n{xref_offset}\n%%EOF\n"
        ).encode("ascii")
    )
    path.write_bytes(b"".join(chunks))


def new_page(pdf: Pdf, title: str, subtitle: str = "") -> None:
    pdf.color(1, 1, 1, stroke=False, fill=True)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, title, 22, "F2")
    if subtitle:
        pdf.text(36, 548, subtitle, 9)


def draw_wrapped(pdf: Pdf, x: float, y: float, text: str, chars: int, size: float = 7.5, font: str = "F1") -> float:
    line_h = size + 2
    for line in wrap(text, width=chars) or [""]:
        pdf.text(x, y, line, size, font)
        y -= line_h
    return y


def draw_table(pdf: Pdf, x: float, y: float, col_w: list[float], headers: list[str], rows: list[list[str]]) -> float:
    total = sum(col_w)
    pdf.color(0.12, 0.15, 0.19, stroke=False, fill=True)
    pdf.rect(x, y - 19, total, 23, fill=True, stroke=False)
    cx = x
    for width, header in zip(col_w, headers):
        pdf.text(cx + 5, y - 11, header, 8, "F2", (1.0, 1.0, 1.0))
        cx += width
    y -= 28

    for idx, row in enumerate(rows):
        line_counts = [max(1, len(wrap(cell, max(9, int(width / 4.7))))) for cell, width in zip(row, col_w)]
        row_h = max(line_counts) * 9.8 + 7
        if idx % 2 == 0:
            pdf.color(0.955, 0.970, 0.985, stroke=False, fill=True)
            pdf.rect(x, y - row_h + 7, total, row_h, fill=True, stroke=False)
        pdf.color(0.78, 0.80, 0.84, stroke=True, fill=False)
        pdf.raw("0.35 w")
        pdf.rect(x, y - row_h + 7, total, row_h, fill=False, stroke=True)
        cx = x
        for cell, width in zip(row, col_w):
            draw_wrapped(pdf, cx + 5, y, cell, max(9, int(width / 4.7)), 7.4)
            cx += width
        y -= row_h
    return y


def main() -> None:
    pdf = Pdf()

    new_page(
        pdf,
        "SG98 Remote / Controller Connection Matrix",
        "Final live regression run: 2026-04-26 PDT, generic remote + controller on the bench ports.",
    )
    pdf.text(
        36,
        526,
        "Primary rule: ESP-NOW stays available in every normal connectivity state. Wi-Fi enabled adds AP/web access beside ESP-NOW during setup/control, then parks back to ESP-NOW after home Wi-Fi is online, internet sync is valid, and ESP-NOW is healthy.",
        9.2,
        "F2",
    )
    draw_table(
        pdf,
        36,
        492,
        [130, 150, 420],
        ["Area", "Final Rule", "Recovery Meaning"],
        [
            ["Remote", "ESP-NOW first", "Factory/no-info remote scans and locks a controller without requiring saved Wi-Fi."],
            ["Controller default", "Wi-Fi disabled", "Default internal state is ESP-NOW-only; Wi-Fi/AP is not a parked state."],
            ["Wi-Fi enabled", "WEB+ESP", "Controller AP/web UI is available while ESP-NOW stays online; after home Wi-Fi joins and sync completes, it parks back to ESP-NOW."],
            ["Full-size only", "Radio off allowed", "A standalone controller can choose Radio off because the local UI can recover it."],
            ["Headless", "Radio off rejected", "Headless also rejects Wi-Fi-only, then opens WEB+ESP recovery if ESP-NOW traffic is missing."],
        ],
    )
    draw_table(
        pdf,
        36,
        326,
        [175, 110, 415],
        ["Build / Flash", "Result", "Notes"],
        [
            ["remote-universal-s3", "PASS + flashed", "Loaded to /dev/cu.usbmodem21101 after Wi-Fi enabled/disabled wording change."],
            ["controller-universal-s3", "PASS + flashed", "Loaded to /dev/cu.usbmodem21201 before and after headless-sim testing; hardware is left on this image."],
            ["controller-universal-s3-headless-sim", "PASS + flashed/tested", "Used only for live headless recovery verification, then replaced by full-size firmware."],
            ["dual_board_regression.py", "PASS", "Both full-size and headless-sim matrices passed from factory/no-info reset."],
        ],
    )

    pdf.page()
    new_page(pdf, "Operation And Recovery Matrix", "Expected paths for full-size, headless, Wi-Fi setup, and remote loss.")
    draw_table(
        pdf,
        28,
        520,
        [118, 102, 248, 244],
        ["Case", "Start", "Expected Operation", "Recovery / Exit"],
        [
            ["Fresh pair", "Both ESP-NOW", "Remote scans, discovers controller, locks peer, and polls over ESP-NOW. No saved Wi-Fi required.", "Final state is ESP-NOW-only with Wi-Fi/AP off."],
            ["Wi-Fi enabled", "WEB+ESP", "AP/home web UI is available; remote still controls over ESP-NOW.", "Keep AP/web up through setup, then park to ESP-NOW after home Wi-Fi is online, synced, and the ESP-NOW link is healthy."],
            ["Full-size only", "ESP-NOW", "Controller stays locally usable with no remote present.", "User can enable Wi-Fi or choose Radio off locally."],
            ["Headless only", "ESP-NOW", "Controller waits for ESP-NOW traffic.", "If silent for the timeout, open WEB+ESP recovery AP; close recovery bridge after relock and quiet web access."],
            ["Remote lost", "Was healthy", "Headless controller tracks command/status traffic as activity.", "If traffic stays missing, stop unsafe running states first, then open WEB+ESP recovery."],
            ["Wi-Fi-only request", "WEB requested", "Full-size diagnostic/bench state only.", "Headless rejects it because ESP-NOW would be unavailable."],
            ["Radio off", "OFF requested", "Allowed only for full-size standalone use.", "Headless rejects/sanitizes it so the controller cannot strand itself."],
        ],
    )

    pdf.page()
    new_page(pdf, "Full-Size Live Results", "Normal controller build with screen, buttons, encoder, and local recovery enabled.")
    draw_table(
        pdf,
        28,
        520,
        [145, 100, 112, 112, 242],
        ["Case", "Remote Avg/Max", "Controller Avg/Max", "Result", "Observed Behavior"],
        [
            ["Factory/no-info reset", "-", "-", "PASS", "Remote network reset + peer clear, controller network reset, no saved Wi-Fi required."],
            ["Fresh ESP-NOW discovery", "7.8 / 11.2 ms", "103.8 / 247.1 ms", "PASS", "Remote locked controller over ESP-NOW; remote Wi-Fi inactive."],
            ["Wi-Fi enabled bridge", "14.4 / 41.3 ms", "92.6 / 148.9 ms", "PASS", "Controller stayed WEB+ESP while AP/web access was active; remote stayed ESP-NOW."],
            ["Controller-only local use", "-", "-", "PASS", "With remote ESP-NOW silent, full-size controller remained locally usable."],
            ["Radio off full-size", "-", "-", "PASS", "OFF was allowed because local controls can recover the controller."],
            ["ESP-NOW parked", "12.2 / 68.0 ms", "97.6 / 224.2 ms", "PASS", "Explicit Wi-Fi disabled returned to ESP-NOW-only and remote Wi-Fi stayed off."],
            ["Full summary", "count 135, avg 8.1, p95 21.4, max 68.0 ms", "count 105, avg 148.0, p95 335.6, max 462.3 ms", "PASS", "ALL_DUAL_BOARD_REGRESSION_PASS"],
        ],
    )

    pdf.page()
    new_page(pdf, "Headless-Sim Live Results", "Same physical controller with local UI capabilities disabled for recovery testing.")
    draw_table(
        pdf,
        28,
        520,
        [145, 100, 112, 112, 242],
        ["Case", "Remote Avg/Max", "Controller Avg/Max", "Result", "Observed Behavior"],
        [
            ["Factory/no-info reset", "-", "-", "PASS", "Headless-sim started from no saved Wi-Fi and no locked peer."],
            ["Fresh ESP-NOW discovery", "8.6 / 11.7 ms", "87.5 / 96.7 ms", "PASS", "Remote found and locked the controller without Wi-Fi credentials."],
            ["Wi-Fi enabled bridge", "7.9 / 10.8 ms", "83.3 / 101.9 ms", "PASS", "WEB+ESP kept ESP-NOW online while recovery/web access was available."],
            ["ESP-NOW fallback", "7.5 / 11.2 ms", "96.1 / 98.9 ms", "PASS", "When remote ESP-NOW traffic went silent, controller opened WEB+ESP recovery."],
            ["Headless guard", "9.6 / 24.5 ms", "86.7 / 101.0 ms", "PASS", "OFF and Wi-Fi-only were rejected so the headless controller stayed reachable."],
            ["ESP-NOW parked", "8.4 / 13.8 ms", "89.1 / 97.6 ms", "PASS", "Returned remote relocked over ESP-NOW; parked state remained low-resource."],
            ["Headless summary", "count 163, avg 12.9, p95 57.5, max 119.1 ms", "count 167, avg 108.6, p95 174.2, max 278.3 ms", "PASS", "ALL_DUAL_BOARD_REGRESSION_PASS"],
        ],
    )

    pdf.page()
    new_page(pdf, "Corner Cases Covered", "What was specifically checked so a controller is not stranded.")
    draw_table(
        pdf,
        36,
        520,
        [170, 230, 300],
        ["Risk", "Tested Path", "Resolution"],
        [
            ["Factory-fresh with no Wi-Fi", "Remote/controller reset network; WebSocket suite intentionally skipped.", "Remote can recover through ESP-NOW discovery without any saved SSID."],
            ["Headless boots without remote", "Remote transport forced away from ESP-NOW until timeout.", "Controller opens WEB+ESP recovery AP."],
            ["Headless user selects Radio off", "API/menu radio OFF request.", "Request rejected with remoteRecoveryRequired=true."],
            ["Headless user selects Wi-Fi only", "API/menu WEB-only request.", "Request rejected; WEB+ESP is the supported recovery bridge."],
            ["Full-size standalone owner", "Remote ESP-NOW disabled while controller is local.", "Controller does not force AP recovery because local screen/buttons/encoder exist."],
            ["Wi-Fi/web UI active", "User-selected WEB+ESP bridge with AP/web activity.", "Bridge remains open through home setup; automatic park happens after online sync and a healthy ESP-NOW link. Recovery bridges still wait for quiet web access."],
        ],
    )
    pdf.text(36, 64, "Report artifact: Documentation/remote-controller-connection-matrix-report.pdf", 8.5, "F2")
    pdf.text(36, 48, "Source matrix: Documentation/remote-controller-connection-matrix.md", 8.5, "F2")

    write_pdf(pdf, OUT)
    print(OUT)


if __name__ == "__main__":
    main()
