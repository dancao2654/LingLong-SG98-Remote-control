#include <cassert>
#include <cstdio>
#include "sg98_connection_reveal.h"

using sg98_connection_reveal::State;

int main() {
    State s;
    assert(!s.observe(false, true, 10));
    assert(s.observe(true, true, 20));
    assert(s.active && s.count == 1);
    assert(!s.observe(true, true, 21));
    s.service(true, false, 1519);
    assert(s.active);
    s.service(true, false, 1520);
    assert(!s.active);
    s.observe(false, true, 1600);
    assert(!s.observe(true, true, 1700)); // Brief packet loss must not replay.
    s.observe(false, true, 1800);
    s.observe(false, true, 6800);
    assert(s.observe(true, true, 6801));
    s.service(true, true, 6802);
    assert(!s.active); // Input immediately dismisses the animation.
    s.observe(false, true, 7000);
    s.observe(false, true, 12000);
    assert(!s.observe(true, false, 12001)); // OTA/grinding/menu reconnect is consumed silently.
    assert(!s.observe(true, true, 12002));
    assert(s.count == 2);
    State wrap;
    wrap.observe(true, true, UINT32_MAX - 100);
    wrap.service(true, false, 1400);
    assert(!wrap.active);
    std::puts("PASS: connection reveal edges, debounce, suppression, dismissal and wraparound");
}
