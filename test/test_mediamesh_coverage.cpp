// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh coverage sums (src/helpers/MediaMeshCoverage.h). The margins are
// the same golden values MeshCore Open TAK's
// test/services/mediamesh_coverage_test.dart checks.

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCoverage.h"

using namespace mmcov;

static bool near(float a, float b, float eps = 1e-3f) { return fabsf(a - b) < eps; }

int main() {
  auto m = [](float snr, uint32_t bw2, uint8_t sf) { return margin(snr, 62500, bw2, sf); };
  assert(near(m(3, 250000, 7), 4.4794f));
  assert(near(m(-2, 250000, 7), -0.5206f));
  assert(near(m(3, 125000, 7), 7.4897f));
  assert(near(m(0, 62500, 9), 12.5f, 1e-6f));
  assert(reachFor(true, m(3, 250000, 7)) == Marginal);
  assert(reachFor(true, m(-2, 250000, 7)) == Out);
  assert(reachFor(true, m(3, 125000, 7)) == Good);
  assert(reachFor(false, 99) == None);
  assert(near(snrFloor(12), -20.0f));

  // The weaker direction counts.
  assert(near(linkSnr(18, 12), 3.0f));
  assert(near(linkSnr(-8, 12), -2.0f));

  // Sample packing.
  Sample s;
  s.count = 1;
  s.best_q4 = toQ4(4.48f);
  assert(s.best_q4 == 18 && s.reach() == Marginal);
  assert(toQ4(100) == 127 && toQ4(-100) == -128);
  Sample none;
  assert(none.reach() == None);

  // Distance and the ask schedule: ~111 m per 0.001 deg of latitude.
  const double d = metresBetween(47600000, -122300000, 47601000, -122300000);
  assert(d > 110 && d < 112);
  assert(due(false, 0, 0, 0, 1, 1, 0));
  assert(!due(true, 47600000, -122300000, 0, 47602000, -122300000, 30000));   // moved, too soon
  assert(due(true, 47600000, -122300000, 0, 47602000, -122300000, 41000));    // moved, long enough
  assert(!due(true, 47600000, -122300000, 0, 47601000, -122300000, 90000));   // 111 m: not far enough
  assert(due(true, 47600000, -122300000, 4294960000u, 47602000, -122300000, 50000));   // millis wrap

  // CSV.
  char line[160];
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, Marginal, 4.48f, "First Hill", 4.5f, 3.0f, 4.48f);
  assert(strcmp(line, "1760000000,47.600000,-122.300000,marginal,4.5,First Hill,4.5,3.0,4.5\n") == 0);
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, Marginal, 4.48f, "First Hill", 4.5f, 3.0f, 4.48f, "1ea5e195");
  assert(strcmp(line, "1760000000,47.600000,-122.300000,marginal,4.5,First Hill,4.5,3.0,4.5,1ea5e195\n") == 0);
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, None, 0, nullptr, 0, 0, 0);
  assert(strcmp(line, "1760000000,47.600000,-122.300000,none,,,,,\n") == 0);
  // RSSI, an answer on radio2 and the repeaters asked on radio2 that didn't answer
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, Good, 13.0f, "First Hill", 11.5f, 12.0f, 13.0f, "1ea5e195",
          -64, false, "fba9fe12;5d01cacf");
  assert(strcmp(line, "1760000000,47.600000,-122.300000,good,13.0,First Hill,11.5,12.0,13.0,1ea5e195,-64,,fba9fe12;5d01cacf\n") == 0);
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, Good, 13.0f, "SoDo", 2.0f, 1.0f, 8.5f, "5491c13d", 0, true);
  assert(strcmp(line, "1760000000,47.600000,-122.300000,good,13.0,SoDo,2.0,1.0,8.5,5491c13d,,radio2,\n") == 0);
  csvLine(line, sizeof line, 1760000000, 47600000, -122300000, None, 0, nullptr, 0, 0, 0, nullptr, 0, false, "fba9fe12");
  assert(strcmp(line, "1760000000,47.600000,-122.300000,none,,,,,,,,,fba9fe12\n") == 0);
  assert(kAskMax == 3 && kAskWaitMs < kMinGapMs);
  assert(reachRgb(Good) == 0x22C55E && reachRgb(None) == 0x6B7280);

  printf("mediamesh coverage: ok\n");
  return 0;
}
