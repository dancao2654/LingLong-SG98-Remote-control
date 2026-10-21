#!/usr/bin/env python3
"""Host-test the firmware's actual connection label and redraw functions."""
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature + " {")
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    source = (Path(__file__).resolve().parents[1] / "sg98-remote/src/remote_app.cpp").read_text()
    functions = "\n".join(function(source, signature) for signature in (
        "bool isLinkOnline()",
        "String controllerConnectionText()",
        "void serviceNetworkStatusRedraw()",
    ))
    fixture = r'''
#include <cassert>
#include <cstdint>
#include <string>
using String = std::string;
enum class LinkTransport { None, WebSocket, EspNow };
struct {
    LinkTransport currentTransport = LinkTransport::None;
    uint32_t lastLinkActivityAt = 0;
    bool redraw = false;
} app;
namespace remote_config { constexpr uint32_t kLinkTimeoutMs = 5000; }
uint32_t now = 100;
uint32_t millis() { return now; }
String network = "NETWORK OFF";
String toast;
String networkCurveStatusText() { return network; }
String roundHomeBottomLabel() { return toast.empty() ? network : toast; }
'''
    checks = r'''
int main() {
    assert(!isLinkOnline());
    assert(controllerConnectionText() == "NET CTRL OFFLINE");
    app.currentTransport = LinkTransport::WebSocket;
    network = "WIFI LAN 10.0.0.113";
    assert(controllerConnectionText() == "WIFI CTRL OFFLINE");
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    app.redraw = false;
    serviceNetworkStatusRedraw();
    assert(!app.redraw);
    app.lastLinkActivityAt = now;
    assert(controllerConnectionText() == "WIFI CTRL ONLINE");
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    now += 4999;
    app.redraw = false;
    serviceNetworkStatusRedraw();
    assert(!app.redraw);
    ++now;
    assert(!isLinkOnline());
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    assert(controllerConnectionText() == "WIFI CTRL OFFLINE");
    app.currentTransport = LinkTransport::EspNow;
    network = "ESP-NOW SEARCH";
    assert(controllerConnectionText() == "ESP-NOW CTRL OFFLINE");
    app.redraw = false;
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    app.lastLinkActivityAt = now;
    assert(controllerConnectionText() == "ESP-NOW CTRL ONLINE");
    serviceNetworkStatusRedraw();
    app.redraw = false;
    network = "ESP-NOW LINK";
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    app.redraw = false;
    toast = "MENU SAVED";
    serviceNetworkStatusRedraw();
    assert(app.redraw);
    // Link freshness remains correct across the millis() wrap boundary.
    app.lastLinkActivityAt = UINT32_MAX - 100;
    now = 100;
    assert(isLinkOnline());
    now = 5000;
    assert(!isLinkOnline());
}
'''
    with tempfile.TemporaryDirectory(prefix="sg98-connection-test-") as directory:
        test = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        test.write_text(fixture + functions + checks)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: Wi-Fi/ESP-NOW labels, timeout, wraparound, and redraw transitions")


if __name__ == "__main__":
    main()
