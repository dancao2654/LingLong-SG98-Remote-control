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

    def text(
        self,
        x: float,
        y: float,
        text: str,
        size: float = 9,
        font: str = "F1",
        fill_color: tuple[float, float, float] | None = (0.10, 0.12, 0.15),
    ) -> None:
        if fill_color is not None:
            self.color(*fill_color, fill=True, stroke=False)
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


def draw_wrapped(pdf: Pdf, x: float, y: float, text: str, width_chars: int, size: float = 8, font: str = "F1") -> float:
    line_h = size + 2
    for line in wrap(text, width=width_chars) or [""]:
        pdf.text(x, y, line, size, font)
        y -= line_h
    return y


def draw_table(pdf: Pdf, x: float, y: float, col_w: list[float], headers: list[str], rows: list[list[str]]) -> float:
    row_gap = 6
    pdf.color(0.10, 0.12, 0.15, fill=True, stroke=False)
    pdf.rect(x, y - 20, sum(col_w), 24, fill=True, stroke=False)
    cx = x
    for width, header in zip(col_w, headers):
        pdf.text(cx + 6, y - 12, header, 8, "F2", fill_color=(1.0, 1.0, 1.0))
        cx += width
    y -= 28
    for idx, row in enumerate(rows):
        line_counts = [max(1, len(wrap(cell, max(10, int(width / 4.8))))) for cell, width in zip(row, col_w)]
        row_h = max(line_counts) * 10 + row_gap
        pdf.color(0.965, 0.975, 0.985, fill=True, stroke=False)
        if idx % 2 == 0:
            pdf.rect(x, y - row_h + 8, sum(col_w), row_h, fill=True, stroke=False)
        pdf.color(0.78, 0.80, 0.84, stroke=True)
        pdf.line_width(0.4)
        pdf.rect(x, y - row_h + 8, sum(col_w), row_h, fill=False, stroke=True)
        cx = x
        for cell, width in zip(row, col_w):
            draw_wrapped(pdf, cx + 6, y, cell, max(10, int(width / 4.8)), 7.5)
            cx += width
        y -= row_h
    return y


def main() -> None:
    pdf = Pdf()
    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)

    pdf.text(36, 568, "SG98 Remote / Controller Connection Matrix", 22, "F2")
    pdf.text(36, 548, "Bench run date: 2026-04-26 17:43 PDT", 10)
    pdf.text(36, 532, "Primary goal: no factory-fresh remote/controller pair should strand a headless controller.", 10.5, "F2")

    summary_rows = [
        ["Live remote", "Waveshare AMOLED 1.75 / /dev/cu.usbmodem21101", "Factory/no-info reset before each pass: no saved Wi-Fi and no locked ESP-NOW peer."],
        ["Full-size pass", "Adafruit Feather Controller / controller_box", "Same physical controller with screen, buttons, and encoder enabled. OFF remains allowed because local recovery exists."],
        ["Headless-sim pass", "controller_box_headless_sim", "Same physical controller with board features reporting no screen, no buttons, no encoder, no encoder button."],
        ["Steady state", "ESP-NOW channel 1", "Both passes ended with remote online over ESP-NOW, remote Wi-Fi off, and no saved home Wi-Fi credentials."],
    ]
    draw_table(pdf, 36, 500, [110, 230, 360], ["Area", "Observed / Build", "Meaning"], summary_rows)

    build_rows = [
        ["remote-waveshare-175", "PASS", "Built and flashed to the bench remote."],
        ["controller-universal-s3", "PASS", "Built and flashed for the live full-size controller pass."],
        ["controller-universal-s3-headless-sim", "PASS", "Built and flashed for live headless simulation on the same physical controller."],
        ["waveshare_esp32_s3_rs485_can", "PASS", "Real headless target build still passes; behavior matched by headless simulation feature flags."],
        ["remote-crowpanel-21 / remote-crowpanel-128", "PASS", "Encoder remote builds previously passed with softened menu encoder path."],
    ]
    draw_table(pdf, 36, 312, [230, 70, 400], ["Build Target", "Result", "Notes"], build_rows)

    pdf.page()
    matrix_rows = [
        [
            "Full-size factory reset",
            "No saved SSID, no saved peer.",
            "Remote reset network + peer clear; controller reset network. Remote discovered controller from scratch.",
            "PASS live",
        ],
        [
            "Full-size fresh discovery",
            "ESP-NOW link first, Wi-Fi off.",
            "Remote locked controller and stayed online with remote Wi-Fi inactive and staSaved=false.",
            "PASS live",
        ],
        [
            "Full-size command matrix",
            "Remote/controller sync in both directions.",
            "RPM, weight, menu writes, mode toggles, and soak loop completed over ESP-NOW.",
            "PASS live",
        ],
        [
            "Headless-sim factory reset",
            "No saved SSID, no saved peer.",
            "Remote reset network + peer clear; simulated headless controller reset network and remained recoverable.",
            "PASS live",
        ],
        [
            "Headless-sim fresh discovery",
            "Remote must find controller without Wi-Fi credentials.",
            "Remote locked dual FE:01:2C:D6:38:BC, requested ESP-NOW-only, then settled on FC:01:2C:D6:38:BC.",
            "PASS live",
        ],
        [
            "Headless-sim OFF guard",
            "OFF must be refused when local UI is disabled.",
            "API rejected radio OFF with remoteRecoveryRequired=true; controller stayed recoverable.",
            "PASS live",
        ],
        [
            "Headless-sim command matrix",
            "ESP-NOW remains primary under no-info setup.",
            "RPM, weight, menu writes, mode toggles, and soak loop completed over ESP-NOW.",
            "PASS live",
        ],
        [
            "WebSocket in no-info mode",
            "Do not require Wi-Fi provisioning.",
            "Skipped by design: a factory-fresh remote has no SSID and should not depend on WebSocket to recover.",
            "N/A",
        ],
    ]

    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, "Live Connection Matrix Results", 22, "F2")
    pdf.text(36, 548, "Both passes used the same physical controller; headless mode was simulated by disabling local UI features in board capabilities.", 9)
    draw_table(pdf, 36, 516, [175, 160, 275, 90], ["Case", "Expected", "Verification", "Result"], matrix_rows)

    pdf.page()
    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, "Detailed Test Cases: Full-Size Controller", 22, "F2")
    pdf.text(36, 548, "Normal Adafruit Feather controller build with local screen, buttons, encoder, and encoder button enabled.", 9)

    full_detail_rows = [
        [
            "F1",
            "Factory/no-info reset",
            "Remote reset_network + peer clear. Controller reset_network + idle. Controller radio set to WEB+ESP.",
            "No saved home SSID on either recovery path and no pre-locked ESP-NOW peer.",
            "PASS: remote staSaved=false; controller stayed recoverable on ESP-NOW channel 1.",
        ],
        [
            "F2",
            "Fresh ESP-NOW discovery",
            "Remote set to transport espnow after peer clear.",
            "Remote should find the controller without Wi-Fi credentials, lock the peer, and report linkOnline.",
            "PASS: remote locked FC:01:2C:D6:38:BC and linkOnline=true.",
        ],
        [
            "F3",
            "Low-resource steady state",
            "Let controller settle after discovery.",
            "Once ESP-NOW is effective, Wi-Fi should not be required by the remote.",
            "PASS: remote Wi-Fi inactive, staSaved=false, ESP-NOW online.",
        ],
        [
            "F4",
            "Menu pull",
            "Remote raw GET_MENU=4.",
            "Remote can request and mirror controller menu data over ESP-NOW.",
            "PASS: remote menu 4 data became visible; menu item check succeeded.",
        ],
        [
            "F5",
            "Controller to remote RPM",
            "Controller api setrpm 650.",
            "Remote status should update controller.setRPM and manualRPM.",
            "PASS: remote observed 650 RPM.",
        ],
        [
            "F6",
            "Remote to controller RPM",
            "Remote api setrpm 775.",
            "Controller setRPM should update and remote should reflect it back.",
            "PASS: controller and remote both observed 775 RPM.",
        ],
        [
            "F7",
            "Weight sync both ways",
            "Controller setweight 19000, then remote setweight 21000.",
            "Both sides should converge on the latest target weight in mg.",
            "PASS: controller and remote target weight values matched.",
        ],
        [
            "F8",
            "Menu writes",
            "Remote changed sleep timer, purge auto, and GBW RPM menu values.",
            "Controller menu and remote menu mirrors should both converge.",
            "PASS: all three menu writes were verified on controller and remote.",
        ],
        [
            "F9",
            "State transitions",
            "Remote action idle_gbw, then action idle.",
            "Controller state should follow remote commands and remote status should mirror state.",
            "PASS: IDLE_GBW then IDLE were both observed.",
        ],
        [
            "F10",
            "ESP-NOW soak",
            "10 iterations of remote RPM writes plus controller weight writes.",
            "Link should remain ESP-NOW and linkOnline through repeated updates.",
            "PASS: no link loss during soak.",
        ],
        [
            "F11",
            "WebSocket under no-info",
            "Skipped in factory/no-info run.",
            "Fresh devices have no saved Wi-Fi; recovery must not depend on WebSocket.",
            "N/A by design for this matrix.",
        ],
    ]
    draw_table(pdf, 28, 516, [32, 95, 190, 205, 230], ["ID", "Case", "Stimulus", "Expected", "Observed"], full_detail_rows)

    pdf.page()
    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, "Detailed Test Cases: Headless-Sim Controller", 22, "F2")
    pdf.text(36, 548, "Same physical controller flashed with controller_box_headless_sim, reporting no local screen/buttons/encoder.", 9)

    headless_detail_rows = [
        [
            "H1",
            "Headless feature profile",
            "Flash controller-universal-s3-headless-sim.",
            "Controller target should report headless simulation so recovery rules treat local UI as unavailable.",
            "PASS: target controller_box_headless_sim during test pass.",
        ],
        [
            "H2",
            "Factory/no-info reset",
            "Remote reset_network + peer clear. Controller reset_network + idle. Controller radio set to WEB+ESP.",
            "Controller must remain remotely recoverable because there is no local UI fallback.",
            "PASS: controller remained recoverable and remote had no saved Wi-Fi.",
        ],
        [
            "H3",
            "Fresh ESP-NOW discovery",
            "Remote set to transport espnow after peer clear.",
            "Remote should find the headless controller without any saved Wi-Fi and lock the peer.",
            "PASS: remote discovered the controller and came online over ESP-NOW.",
        ],
        [
            "H4",
            "Dual to ESP-NOW settle",
            "Controller moved from recovery/dual advertisement toward ESP-NOW-only steady state.",
            "Remote may see the temporary FE:01:2C:D6:38:BC dual MAC, then settle on FC:01:2C:D6:38:BC.",
            "PASS: final locked peer was FC:01:2C:D6:38:BC and linkOnline=true.",
        ],
        [
            "H5",
            "OFF guard",
            "Controller api set 1 4 2 while remoteRecoveryRequired=true.",
            "Radio OFF must be rejected so a headless controller is not stranded.",
            "PASS: command returned ok=false with headless rejection; desired mode stayed non-OFF.",
        ],
        [
            "H6",
            "Menu pull",
            "Remote raw GET_MENU=4.",
            "Remote can request controller menu data over ESP-NOW after headless recovery.",
            "PASS: remote menu data check succeeded.",
        ],
        [
            "H7",
            "RPM and weight sync",
            "Same controller-to-remote and remote-to-controller RPM/weight checks as full-size.",
            "Both sides should converge without WebSocket and without local UI.",
            "PASS: setRPM and setWeight paths worked both directions.",
        ],
        [
            "H8",
            "Menu writes",
            "Remote changed sleep timer, purge auto, and GBW RPM menu values.",
            "Controller and remote menu mirrors should both converge in headless mode.",
            "PASS: all menu writes were verified.",
        ],
        [
            "H9",
            "State transitions",
            "Remote action idle_gbw, then action idle.",
            "Headless controller should still follow state commands over ESP-NOW.",
            "PASS: IDLE_GBW then IDLE were both observed.",
        ],
        [
            "H10",
            "ESP-NOW soak",
            "10 iterations of remote RPM writes plus controller weight writes.",
            "Link should remain ESP-NOW and linkOnline through repeated updates.",
            "PASS: no persistent link loss during soak.",
        ],
        [
            "H11",
            "Restore bench unit",
            "Reflash controller-universal-s3 after headless-sim pass.",
            "Physical controller should return to normal full-size firmware.",
            "PASS: target controller_box, board Adafruit Feather Controller, remoteRecoveryRequired=false.",
        ],
    ]
    draw_table(pdf, 28, 516, [32, 95, 190, 205, 230], ["ID", "Case", "Stimulus", "Expected", "Observed"], headless_detail_rows)

    pdf.page()
    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, "Recovery Paths And Issue Notes", 22, "F2")
    pdf.text(36, 548, "This page records what should happen when a case hits a fault or ambiguous transition.", 9)

    recovery_rows = [
        [
            "Factory no saved Wi-Fi / no saved peer",
            "Controller starts WEB+ESP recovery. Remote starts ESP-NOW scan with no locked peer.",
            "Remote discovers controller, locks peer, establishes ESP-NOW, and final state can drop remote Wi-Fi use.",
            "Verified in full-size and headless-sim passes.",
        ],
        [
            "Remote does not find ESP-NOW controller",
            "Remote has no established ESP-NOW controller. If Wi-Fi is configured, it may try Wi-Fi discovery; with no Wi-Fi it stays idle and waits for scan/setup.",
            "Controller must not turn itself into an unrecoverable OFF state while waiting.",
            "Factory/no-info run verified the no-Wi-Fi branch does not require WebSocket.",
        ],
        [
            "Headless controller receives radio OFF request",
            "API/menu request tries to set radio mode OFF while remoteRecoveryRequired=true.",
            "Reject OFF, keep desired mode recoverable, and keep ESP-NOW/recovery path alive.",
            "Verified: api set 1 4 2 returned ok=false and desired mode stayed non-OFF.",
        ],
        [
            "Full-size controller receives radio OFF request",
            "Local screen/buttons/encoder are present.",
            "OFF may remain allowed because the user has a local recovery path on the controller.",
            "Not part of no-strand headless failure path; guard intentionally only applies when local UI is disabled.",
        ],
        [
            "Dual MAC to ESP-NOW MAC transition",
            "During headless recovery, remote can first observe FE:01:2C:D6:38:BC and then settle to FC:01:2C:D6:38:BC.",
            "A transient offline sample should trigger a bounded relock wait rather than marking the controller stranded.",
            "Observed once in early headless load probe; harness updated to wait up to 10s and final rerun passed.",
        ],
        [
            "Earlier WebSocket failure",
            "One obsolete run mixed a controller with no saved STA and a remote with a saved home SSID.",
            "Reset both devices for a real factory/no-info test and skip WebSocket because no saved Wi-Fi exists.",
            "Resolved by factory reset path; not counted as a product failure.",
        ],
    ]
    draw_table(pdf, 36, 516, [175, 210, 210, 105], ["Scenario", "Trigger", "Recovery Path", "Bench Result"], recovery_rows)

    pdf.page()
    pdf.color(1, 1, 1, fill=True, stroke=False)
    pdf.rect(0, 0, 792, 612, fill=True, stroke=False)
    pdf.text(36, 568, "Load / Hogging Check", 22, "F2")
    pdf.text(36, 548, "Serial/API round-trip timings during the live matrix. Max threshold used by the harness: 1500 ms.", 9)

    load_rows = [
        ["Full-size fresh ESP-NOW discovery", "Remote", "12", "32.1 ms", "130.9 ms", "PASS"],
        ["Full-size fresh ESP-NOW discovery", "Controller", "12", "141.1 ms", "325.5 ms", "PASS"],
        ["Full-size ESP-NOW command suite", "Remote", "12", "17.5 ms", "133.7 ms", "PASS"],
        ["Full-size ESP-NOW command suite", "Controller", "12", "119.7 ms", "293.4 ms", "PASS"],
        ["Headless-sim fresh ESP-NOW discovery", "Remote", "12", "24.9 ms", "110.8 ms", "PASS"],
        ["Headless-sim fresh ESP-NOW discovery", "Controller", "12", "97.3 ms", "165.4 ms", "PASS"],
        ["Headless-sim OFF guard", "Remote", "12", "9.4 ms", "40.5 ms", "PASS"],
        ["Headless-sim OFF guard", "Controller", "12", "93.2 ms", "135.6 ms", "PASS"],
        ["Headless-sim ESP-NOW command suite", "Remote", "12", "21.3 ms", "107.7 ms", "PASS"],
        ["Headless-sim ESP-NOW command suite", "Controller", "12", "81.6 ms", "151.0 ms", "PASS"],
    ]
    draw_table(pdf, 36, 516, [245, 85, 55, 85, 85, 65], ["Probe", "Side", "N", "Avg", "Max", "Result"], load_rows)

    summary_timing_rows = [
        ["Full-size no-info pass", "Remote", "114", "18.9 ms", "113.5 ms", "133.7 ms"],
        ["Full-size no-info pass", "Controller", "78", "145.5 ms", "335.7 ms", "343.1 ms"],
        ["Headless-sim no-info pass", "Remote", "141", "13.8 ms", "88.5 ms", "111.8 ms"],
        ["Headless-sim no-info pass", "Controller", "83", "101.0 ms", "222.0 ms", "290.0 ms"],
    ]
    draw_table(pdf, 36, 230, [230, 85, 70, 95, 95, 95], ["Overall Pass", "Side", "Count", "Avg", "P95", "Max"], summary_timing_rows)

    pdf.text(36, 72, "Result:", 10, "F2")
    pdf.text(36, 56, "1. No load probe exceeded the 1500 ms guard threshold; no periodic hogging was observed in this matrix.", 8.5)
    pdf.text(36, 42, "2. Factory/no-info WebSocket was intentionally skipped; recovery must not depend on saved Wi-Fi.", 8.5)
    pdf.text(36, 28, "3. Follow-up UI notes captured separately: remote centering across screens and clock temperature suffix.", 8.5)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    write_pdf(pdf, OUT)
    print(OUT)


if __name__ == "__main__":
    main()
