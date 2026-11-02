// SPDX-License-Identifier: GPL-3.0-or-later
// Transit cards on watched channels: TransitFeed.h against BusBot's replies
// (the same samples MeshCore Open TAK's transit_watch test uses).
//
//   g++ -std=c++17 -Wall -I src test/test_transit_feed.cpp -o /tmp/t && /tmp/t
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui-touch/TransitFeed.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_fail++; } } while (0)
#define CONTAINS(a, b) do { if (!strstr((a), (b))) { fprintf(stderr, "FAIL %s:%d  \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_fail++; } } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-6; }

using namespace transitfeed;

// 2026-10-27 08:10 UTC (the test runs with TZ=UTC)
static const uint32_t kSent = 1793088600UL;

static void stopReply() {
  const char* t =
      "\xF0\x9F\x9A\x8F 3rd Ave & Pike St, Bay 2\n"
      "\xF0\x9F\x9A\x8C 11 - 5 mins \xF0\x9F\x95\x92\n"
      "\xF0\x9F\x9A\x8D G Line - now\n"
      "\xE2\x9B\xB4\xEF\xB8\x8F Seattle-Bainbridge - 21:05 \xF0\x9F\x95\x92\n"
      "\xF0\x9F\x93\xB4 offline: schedule times only\n"
      "also: ct 1120\n"
      "\xF0\x9F\x93\x8D 47.60000,-122.30000 \xC2\xB7 1120";
  Reply r;
  CHECK(parse(t, r));
  CHECK(r.kind == kStop);
  CHECK_STR(r.title, "3rd Ave & Pike St, Bay 2");
  CHECK_STR(r.agency, "");
  CHECK_STR(r.stop_ref, "1120");
  CHECK(r.has_pos && near(r.lat, 47.6) && near(r.lon, -122.3));
  CHECK(r.offline);
  CHECK_STR(r.also, "ct 1120");
  CHECK(r.n_arr == 3);
  CHECK_STR(r.arr[0].route, "11");
  CHECK(r.arr[0].mode == kBus && r.arr[0].minutes == 5 && r.arr[0].scheduled);
  CHECK_STR(r.arr[1].route, "G Line");
  CHECK(r.arr[1].mode == kRapid && r.arr[1].minutes == 0 && !r.arr[1].scheduled);
  CHECK_STR(r.arr[2].route, "Seattle-Bainbridge");
  CHECK(r.arr[2].mode == kFerry && r.arr[2].hh == 21 && r.arr[2].mm == 5);
  CHECK_STR(statusWord(r), "BUS");
  CHECK(level(r) == watchfeed::kNone);   // offline

  char out[600];
  describe(r, 180, kSent, true, out, sizeof out);
  CONTAINS(out, "Stop 1120 \xC2\xB7 King County Metro");
  CONTAINS(out, "Bus 11  2 min \xC2\xB7 8:15  timetable");
  CONTAINS(out, "RapidRide G Line  gone \xC2\xB7 8:10  live");
  CONTAINS(out, "Ferry Seattle-Bainbridge  12 h 52 \xC2\xB7 21:05  timetable");
  CONTAINS(out, "Offline: timetable times");
  CONTAINS(out, "Same stop number at: ct 1120");
  describe(r, 0, 0, false, out, sizeof out);   // no clock on the radio
  CONTAINS(out, "11  5 min\nG Line  due\nSeattle-Bainbridge  ");

  watchfeed::Report w;
  CHECK(toReport(r, w));
  CHECK_STR(w.title, "3rd Ave & Pike St, Bay 2");
  CHECK(near(w.lat, 47.6));
}

static void routeReply() {
  Reply r;
  CHECK(parse("\xF0\x9F\x9A\x8C 201 @ Everett Station Bay 3 \xC2\xB7 CT\n5 mins \xF0\x9F\x95\x92\n17 mins \xF0\x9F\x95\x92\n"
              "\xF0\x9F\x93\x8D 47.97500,-122.19700 \xC2\xB7 ct 1120", r));
  CHECK(r.kind == kRoute);
  CHECK_STR(r.route, "201");
  CHECK_STR(r.title, "Everett Station Bay 3");
  CHECK_STR(r.agency, "ct");
  CHECK_STR(agencyName(r.agency), "Community Transit");
  CHECK_STR(r.stop_ref, "ct 1120");
  CHECK(r.n_arr == 2 && r.arr[1].minutes == 17 && r.arr[1].mode == kBus);
  CHECK_STR(r.arr[1].route, "201");
  char h[48];
  headline(r, h, sizeof h);
  CHECK_STR(h, "201 @ Everett Station Bay 3");
}

static void alerts() {
  Reply r;
  CHECK(parse("\xF0\x9F\x9A\x8F 3rd Ave & Pike St, Bay 2\n\xE2\x9A\xA0\xEF\xB8\x8F Route 11 detour on Pine St\n"
              "\xF0\x9F\x93\x8D 47.60000,-122.30000 \xC2\xB7 1120", r));
  CHECK(r.kind == kAlerts);
  CHECK(r.n_alerts == 1);
  CHECK_STR(r.alerts[0], "Route 11 detour on Pine St");
  CHECK(level(r) == watchfeed::kActive);
  CHECK(parse("\xF0\x9F\x9A\x8F Stop\n\xE2\x9C\x85 No alerts.", r) && r.alerts_clear && r.kind == kAlerts);
  CHECK(parse("\xF0\x9F\x9A\x8F Stop\n\xF0\x9F\x93\xB4 Can't reach live alerts right now.", r) && r.alerts_unknown);
  // the bot's own alert post, only on a transit channel
  CHECK(parse("\xE2\x9A\xA0\xEF\xB8\x8F CT 201: Detour on Hwy 99", r, true) && r.kind == kAlertPost);
  CHECK_STR(r.title, "CT 201");
  CHECK_STR(r.alerts[0], "Detour on Hwy 99");
  CHECK(!parse("\xE2\x9A\xA0\xEF\xB8\x8F CT 201: Detour on Hwy 99", r, false));
  CHECK(transitChannel("#transit") && !transitChannel("#icewatch"));
}

static void soundTransit() {
  // stop numbers with letters and underscores ("st S_EV")
  Reply r;
  CHECK(parse("\xF0\x9F\x9A\x8F Everett Station \xC2\xB7 ST\n\xF0\x9F\x9A\x86 N Line - 12 mins\n"
              "\xF0\x9F\x93\x8D 47.97500,-122.19700 \xC2\xB7 st S_EV", r));
  CHECK(r.has_pos);
  CHECK_STR(r.stop_ref, "st S_EV");
  CHECK(r.n_arr == 1 && r.arr[0].mode == kTrain);
  CHECK(parse("\xF0\x9F\x94\x8E everett station:\n\xF0\x9F\x9A\x8F st S_EV Everett Station \xE2\x86\x92 N Line", r));
  CHECK_STR(r.hits[0].ref, "st S_EV");
  CHECK_STR(r.hits[0].name, "Everett Station");
}

static void plan() {
  const char* t =
      "\xF0\x9F\xA7\xAD Arrive 8:25 \xC2\xB7 45 min \xC2\xB7 1 transfer \xC2\xB7 timetable\n"
      "\xF0\x9F\x9A\x8C 11 7:50 11040 \xE2\x86\x92 3rd Ave & Pike St, Bay 2 8:00\n"
      "\xF0\x9F\x9A\xB6 1 min to 1121\n"
      "\xF0\x9F\x9A\x8C 49 8:15 1121 \xE2\x86\x92 Broadway & E Madison St 8:25\n"
      "\xF0\x9F\x93\x8D 47.61000,-122.32000 \xC2\xB7 11040";
  Reply r;
  CHECK(parse(t, r));
  CHECK(r.kind == kPlan);
  CHECK_STR(r.plan_arrive, "8:25");
  CHECK(r.plan_min == 45 && r.plan_transfers == 1);
  CHECK(r.n_legs == 3);
  CHECK_STR(r.legs[0].route, "11");
  CHECK_STR(r.legs[0].dep, "7:50");
  CHECK_STR(r.legs[0].board, "11040");
  CHECK_STR(r.legs[0].to, "3rd Ave & Pike St, Bay 2");
  CHECK_STR(r.legs[0].arr, "8:00");
  CHECK(r.legs[1].walk && r.legs[1].walk_min == 1);
  CHECK_STR(r.legs[1].to, "1121");
  CHECK_STR(r.legs[2].arr, "8:25");
  CHECK_STR(statusWord(r), "TRIP");
  char h[48];
  headline(r, h, sizeof h);
  CHECK_STR(h, "Arrive 8:25 \xC2\xB7 Broadway & E Madison St");
  // sent 7:40: the first bus leaves in 10 min
  char out[600];
  describe(r, 0, kSent - 30 * 60, true, out, sizeof out);
  CONTAINS(out, "Arrive 8:25 (in 45 min) \xC2\xB7 45 min \xC2\xB7 1 transfer \xC2\xB7 timetable");
  CONTAINS(out, "Bus 11 7:50 from 11040, leaves in 10 min\n  to 3rd Ave & Pike St, Bay 2, 8:00");
  CONTAINS(out, "Walk 1 min to stop 1121");
}

static void search() {
  Reply r;
  CHECK(parse("\xF0\x9F\x94\x8E Near Seattle Center:\n"
              "\xF0\x9F\x9A\x8F 11050 Broadway & E Madison St \xC2\xB7 0.6 mi \xE2\x86\x92 49\n"
              "\xF0\x9F\x9A\x8F ct 1120 Everett Station Bay 3 \xE2\x86\x92 201, Swift Blue\n"
              "\xF0\x9F\x93\x8D 47.62000,-122.32000 \xC2\xB7 11050", r));
  CHECK(r.kind == kSearch);
  CHECK_STR(r.title, "Near Seattle Center");
  CHECK(r.n_hits == 2);
  CHECK_STR(r.hits[0].ref, "11050");
  CHECK_STR(r.hits[0].name, "Broadway & E Madison St");
  CHECK_STR(r.hits[0].dist, "0.6 mi");
  CHECK_STR(r.hits[1].ref, "ct 1120");
  CHECK_STR(r.hits[1].name, "Everett Station Bay 3");
  CHECK_STR(r.hits[1].routes, "201, Swift Blue");
}

static void notTransit() {
  Reply r;
  CHECK(!parse("\xF0\x9F\x9F\xA2 OBSERVED\n10-12 15:00\n@Portland NE Killingsworth St\n1-\xF0\x9F\xA5\xBE\n"
               "\xF0\x9F\x93\x8D 45.5625,-122.6108", r));
  CHECK(!parse("meet at the bus stop - now", r));
  CHECK(!parse("Drone at 47.30731, -122.22850 low and slow", r));
  CHECK(!parse("DRONE 1581F9DEV26590296G6G h251m 1m/s https://maps.google.com/?q=47.60378,-122.34052", r));
}

static void splitReply() {
  // the first part has the header, the second the rest and the position
  CHECK(hasHeader("\xF0\x9F\x9A\x8F 3rd Ave & Pike St, Bay 2\n\xF0\x9F\x9A\x8C 11 - 5 mins"));
  const char* part = "\xF0\x9F\x9A\x8D G Line - 18 mins\n\xF0\x9F\x93\x8D 47.60000,-122.30000 \xC2\xB7 1120";
  Reply r;
  CHECK(parse(part, r) && r.kind == kPart);
  CHECK(!hasHeader(part));
  char joined[kTextCap];
  snprintf(joined, sizeof joined, "%s\n%s", "\xF0\x9F\x9A\x8F 3rd Ave & Pike St, Bay 2\n\xF0\x9F\x9A\x8C 11 - 5 mins", part);
  CHECK(parse(joined, r) && r.kind == kStop && r.n_arr == 2 && r.has_pos);
}

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  stopReply();
  routeReply();
  soundTransit();
  alerts();
  plan();
  search();
  notTransit();
  splitReply();
  if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
  printf("transit feed: all passed\n");
  return 0;
}
