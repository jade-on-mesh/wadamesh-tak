// Host test: src/helpers/BatteryGauge.h (T-Deck battery percent).
//   g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I src -o /tmp/t test/test_battery_gauge.cpp
#include <assert.h>
#include <stdio.h>
#include "helpers/BatteryGauge.h"

using namespace battgauge;

int main() {
  // The curve: ends, a knot, between knots, monotonic.
  assert(curvePercent(0) == -1);
  assert(curvePercent(3200) == 0 && curvePercent(3300) == 0);
  assert(curvePercent(4180) == 100 && curvePercent(4250) == 100);
  assert(curvePercent(3800) == 49);
  assert(curvePercent(3750) == 38);   // 42/33 knots, halfway
  for (int mv = 3300, last = 0; mv <= 4200; mv += 5) {
    const int p = curvePercent(mv);
    assert(p >= last);
    last = p;
  }
  // Calibration moves the top: a pack calibrated full at 4.10 V reads 100 there.
  assert(curvePercent(calibratedMv(4100, 4100)) == 100);
  assert(calibratedMv(3800, 0) == 3800);

  // Sag: screen on lifts the reading by 85 mV, so the same cell reads the same %.
  Load on;
  on.active = true;
  Load idle;
  {
    Gauge a, b;
    const int pa = a.update(3715, on, 0);     // 3800 at rest
    const int pb = b.update(3800, idle, 0);
    assert(pa == pb && pa == 49);
  }
  // Going down: only after three readings at least 2 points lower.
  {
    Gauge g;
    assert(g.update(3800, idle, 0) == 49);
    int shown = 49;
    for (int i = 1; i <= 2; ++i) shown = g.update(3700, idle, i * 10000);
    assert(shown == 49);   // the EMA and the three-reading rule hold it
    for (int i = 3; i <= 40; ++i) shown = g.update(3700, idle, i * 10000);
    assert(shown < 49 && shown >= curvePercent(3700));
  }
  // A load sag that goes away doesn't cost percent: the screen goes off, the
  // reading recovers, and the shown value climbs back only after 2 min at rest.
  {
    Gauge g;
    g.update(3900, idle, 0);
    const int start = g.shown;
    int shown = start;
    uint32_t t = 0;
    for (int i = 0; i < 30; ++i) shown = g.update(3750, idle, t += 10000);   // heavy use
    assert(shown < start);
    const int low = shown;
    shown = g.update(3900, idle, t += 10000);
    assert(shown == low);                                // not straight back up
    for (int i = 0; i < 12; ++i) shown = g.update(3900, idle, t += 10000);
    assert(shown > low);                                 // after 2 min at rest
    Gauge busy = g;
    const int before = busy.shown;
    for (int i = 0; i < 30; ++i) busy.update(3900, on, t + (i + 1) * 10000);
    assert(busy.shown == before);                        // never climbs while in use
  }
  printf("battery gauge: ok\n");
  return 0;
}
