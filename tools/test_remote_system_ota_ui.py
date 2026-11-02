#!/usr/bin/env python3
"""Check the remote OTA screen's actual drawing code at both display sizes."""
from pathlib import Path
import subprocess
import tempfile


def function(source, prefix):
    start = source.index(prefix)
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "sg98-remote/src/remote_app.cpp").read_text()
    functions = "\n".join(function(source, prefix) for prefix in (
        "uint8_t systemOtaPercent(",
        "uint8_t systemOtaPhase(",
        "const char* systemOtaHeadline(",
        "bool systemOtaByteProgress(",
        "void drawSystemOtaDevice(",
        "void drawSystemOtaRing(",
        "void drawSystemOtaProgress(",
    ))
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "sg98_system_ota.h"
using std::min;
struct String : std::string {
    using std::string::string;
    String(const std::string& text) : std::string(text) {}
    String(unsigned value) : std::string(std::to_string(value)) {}
};
enum class RemoteTextStyle { Tiny, Small, Medium, Large, Hero };
constexpr uint16_t kColorBg = 1, kColorText = 2, kColorMuted = 3;
constexpr uint16_t kColorGood = 4, kColorAccent2 = 5, kColorPanelHi = 6;
int screenWidth = 466, screenHeight = 466;
bool roundNavigation = true;
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
bool useRoundNavigation() { return roundNavigation; }
struct Layout { int16_t width, height; };
struct {
    sg98_system_ota::Flow flow;
    bool updatePlanKnown = false;
    bool controllerUpdateNeeded = true;
    bool remoteUpdateNeeded = true;
    uint8_t downloadPart = 0;
    uint32_t downloadWritten = 0;
    uint32_t downloadTotal = 0;
    uint32_t transferOffset = 0;
    struct { uint32_t size = 2000000; } controllerManifest;
} g_systemOta;
void checkPoint(int x, int y) {
    assert(x >= 0 && x < screenWidth && y >= 0 && y < screenHeight);
    if(screenWidth != screenHeight) return;
    const int dx = x - screenWidth / 2, dy = y - screenHeight / 2;
    const int radius = min(screenWidth, screenHeight) / 2 - 8;
    assert(dx * dx + dy * dy <= radius * radius);
}
struct RemoteCanvas {
    struct Box { int left, top, right, bottom; };
    std::vector<Box> textBoxes;
    void fillScreen(uint16_t) {}
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t) {
        checkPoint(x0, y0); checkPoint(x1, y1);
    }
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t, uint16_t) {
        checkPoint(x, y); checkPoint(x + w - 1, y + h - 1);
    }
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t, uint16_t) {
        checkPoint(x, y); checkPoint(x + w - 1, y + h - 1);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t) {
        checkPoint(x, y); checkPoint(x + w - 1, y + h - 1);
    }
    void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t) {
        checkPoint(x - r, y); checkPoint(x + r, y);
        checkPoint(x, y - r); checkPoint(x, y + r);
    }
    void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t) { drawCircle(x, y, r, 0); }
    void drawCenteredText(const String& value, int16_t x, int16_t y,
                          RemoteTextStyle style, uint16_t, uint16_t) {
        const int scale = static_cast<int>(style) + 1;
        const int halfWidth = static_cast<int>(value.size()) * 6 * scale / 2;
        const int halfHeight = 8 * scale / 2;
        checkPoint(x - halfWidth, y - halfHeight);
        checkPoint(x + halfWidth, y + halfHeight);
        const Box box{x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight};
        for(const Box& other : textBoxes) {
            assert(box.right <= other.left || other.right <= box.left ||
                   box.bottom <= other.top || other.bottom <= box.top);
        }
        textBoxes.push_back(box);
    }
};
RemoteTextStyle chooseLabelTextStyle(const String& value, int width) {
    for(auto style : {RemoteTextStyle::Medium, RemoteTextStyle::Small, RemoteTextStyle::Tiny}) {
        if(static_cast<int>(value.size()) * 6 * (static_cast<int>(style) + 1) <= width - 16)
            return style;
    }
    return RemoteTextStyle::Tiny;
}
void drawThickLine(RemoteCanvas& canvas, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1, int16_t, uint16_t color) {
    canvas.drawLine(x0, y0, x1, y1, color);
}
'''
    checks = r'''
int main() {
    using sg98_system_ota::Stage;
    assert(systemOtaPhase(Stage::Connect) == 0);
    assert(systemOtaPhase(Stage::DownloadFiles) == 1);
    assert(systemOtaPhase(Stage::ControllerTransfer) == 2);
    assert(systemOtaPhase(Stage::RemoteInstall) == 3);
    assert(systemOtaPercent(2000000, 2000000) == 100);
    for(const auto [width, height] : {std::pair{240, 240}, std::pair{466, 466},
                                     std::pair{480, 480}, std::pair{450, 600}}) {
        screenWidth = width;
        screenHeight = height;
        roundNavigation = width >= 380;
        for(Stage stage : {Stage::Connect, Stage::WifiConnect, Stage::DownloadFiles,
                           Stage::LocalConnect, Stage::LegacyControllerUpdate,
                           Stage::ControllerTransfer, Stage::ControllerVerify,
                           Stage::RemoteInstall}) {
            g_systemOta.flow.stage = stage;
            g_systemOta.updatePlanKnown = stage >= Stage::DownloadFiles;
            g_systemOta.downloadPart = stage == Stage::DownloadFiles ? 2 : 0;
            g_systemOta.downloadTotal = 2000000;
            g_systemOta.downloadWritten = 1000000;
            g_systemOta.transferOffset = 1000000;
            clockMs += 1100;
            RemoteCanvas canvas;
            drawSystemOtaProgress(canvas, {static_cast<int16_t>(width), static_cast<int16_t>(height)});
        }
        for(bool grinderNeeded : {false, true}) {
            for(bool remoteNeeded : {false, true}) {
                g_systemOta.updatePlanKnown = true;
                g_systemOta.controllerUpdateNeeded = grinderNeeded;
                g_systemOta.remoteUpdateNeeded = remoteNeeded;
                g_systemOta.flow.stage = Stage::RemoteInstall;
                g_systemOta.flow.controllerVerified = true;
                RemoteCanvas canvas;
                drawSystemOtaProgress(canvas, {static_cast<int16_t>(width), static_cast<int16_t>(height)});
            }
        }
        g_systemOta.flow.controllerVerified = false;
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="sg98-ota-ui-test-") as directory:
        test = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        test.write_text(fixture + functions + checks)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root / "include"), str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: OTA phases, progress, bounds, and text spacing at 240/466/480/450x600 px")


if __name__ == "__main__":
    main()
