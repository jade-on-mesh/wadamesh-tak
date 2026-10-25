// SPDX-License-Identifier: GPL-3.0-or-later
//
// Battery percent for boards with no fuel-gauge chip, only the cell voltage
// through a divider (the T-Deck family). Adapted from MatterMesh OS's T-Deck
// Plus gauge (tdeck-plus/src/services/battery_gauge.h, GPL-3.0, see NOTICE),
// which was fitted to a 6.8 h discharge log:
//
//  - Load: the cell sags under use, so each reading is lifted by the measured
//    sag of what is on (screen and keyboard light ~85 mV, GPS ~38 mV, Wi-Fi
//    ~26 mV, CPU at full speed while the screen is off ~10 mV) to estimate the
//    rest voltage.
//  - Curve: rest voltage to percent, 0 % at 3.30 V and 100 % at 4.18 V, along
//    the LiPo shape instead of a straight line (which read ~50 % at 3.75 V,
//    where a LiPo has ~40 % left, and fell fastest just when it shouldn't).
//  - Shown percent: goes down one point after three readings at least 2 points
//    lower; goes back up one point per reading only after 2 minutes at rest
//    (screen off, no Wi-Fi or GPS) with three readings at least 3 points
//    higher, so a load sag no longer costs percent for good and normal use
//    doesn't see-saw.
//
// Pure: no Arduino, host-tested (test/test_battery_gauge.cpp).
#pragma once
#include <stdint.h>

namespace battgauge {

struct Load {
  bool active = false;    // screen (and keyboard light) on
  bool wifi = false;
  bool gps = false;
  bool cpu_high = false;  // CPU at full speed
};

inline int loadSagMv(const Load& l) {
  return (l.active ? 85 : 0) + (l.gps ? 38 : 0) + (l.wifi ? 26 : 0) + (l.cpu_high && !l.active ? 10 : 0);
}
inline bool atRest(const Load& l) { return !l.active && !l.wifi && !l.gps; }

// Rest voltage (mV) to percent; -1 for no reading.
inline int curvePercent(int mv) {
  static const int16_t v[] = {3300, 3450, 3550, 3620, 3680, 3730, 3770, 3800, 3830,
                              3870, 3900, 3950, 4000, 4050, 4100, 4150, 4180};
  static const int8_t p[] = {0, 4, 8, 13, 22, 33, 42, 49, 56, 62, 66, 71, 75, 84, 92, 98, 100};
  constexpr int n = sizeof(v) / sizeof(v[0]);
  if (mv <= 0) return -1;
  if (mv <= v[0]) return 0;
  if (mv >= v[n - 1]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv <= v[i]) return p[i - 1] + ((mv - v[i - 1]) * (p[i] - p[i - 1]) + (v[i] - v[i - 1]) / 2) / (v[i] - v[i - 1]);
  }
  return 100;
}

// A user calibration ("this reading is 100 %", full_mv) moves the curve's top,
// 4.18 V, to it: readings are scaled before the curve. 0 = not calibrated.
inline int calibratedMv(int mv, int full_mv) {
  if (full_mv <= 0 || mv <= 0) return mv;
  return (int)((int64_t)mv * 4180 / full_mv);
}

// One reading every 10-20 s on battery. update() returns the percent to show
// (-1 until the first reading).
struct Gauge {
  float rest_mv = 0;
  int shown = -1, low = 0, high = 0, candidate = -1;
  uint32_t rest_since = 0;
  bool resting = false;

  void reset() { *this = Gauge(); }

  int update(int mv, const Load& load, uint32_t now_ms, int full_mv = 0) {
    if (mv <= 0) return shown;
    const float rest = (float)(mv + loadSagMv(load));
    rest_mv = rest_mv <= 0 ? rest : rest_mv + (rest - rest_mv) * 0.15f;   // EMA
    const bool r = atRest(load);
    if (r && !resting) rest_since = now_ms;
    resting = r;
    candidate = curvePercent(calibratedMv((int)(rest_mv + 0.5f), full_mv));
    if (shown < 0) { shown = candidate; low = high = 0; return shown; }
    if (candidate <= shown - 2) {
      high = 0;
      if (++low >= 3) { shown--; low = 0; }
    } else if (candidate >= shown + 3 && resting && (uint32_t)(now_ms - rest_since) >= 120000u) {
      low = 0;
      if (++high >= 3) { shown++; high = 2; }   // then one point per reading
    } else {
      low = high = 0;
    }
    return shown;
  }
};

}  // namespace battgauge
