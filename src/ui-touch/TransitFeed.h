// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Transit cards on watched channels, as in MeshCore Open TAK's Watch tab
// (lib/team/transit_watch.dart, documentation/transit-watch.md).
//
// BusBot (github.com/jade-on-mesh/BusBot-2.0) answers in a channel such as
// #transit with short replies sized for the mesh:
//
//   🚏 3rd Ave & Pike St, Bay 2
//   🚌 11 - 5 mins 🕒
//   🚍 G Line - now
//   📴 offline: schedule times only
//   also: ct 1120
//   📍 47.60000,-122.30000 · 1120
//
// The last line says where the stop is and its number, so the reply goes on
// the map. This reads the reply back into a card: the stop's number and
// agency, each route with its mode, countdowns that keep running from when
// it was sent, live or timetable, alerts, trip plans as steps, stops found.
// A reply split over two messages is put back together by the UI glue
// (TeamUI.inc) before it gets here.
//
// Pure logic, header-only, host-tested in test/test_transit_feed.cpp.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "WatchFeed.h"

namespace transitfeed {

// The joined text of one reply, as kept for its card.
constexpr size_t kTextCap = 360;

enum Kind : uint8_t { kPart, kStop, kRoute, kAlerts, kPlan, kSearch, kAlertPost };
enum Mode : uint8_t { kNoMode, kBus, kRapid, kRail, kTrain, kFerry };

inline const char* modeName(uint8_t m) {
  switch (m) {
    case kRapid: return "RapidRide";
    case kRail: return "Rail";
    case kTrain: return "Train";
    case kFerry: return "Ferry";
    case kBus: return "Bus";
    default: return "";
  }
}

// Agencies BusBot covers, by the code after a stop (" · CT") or before its
// number ("ct 1120"); no code is the bot's default, King County Metro.
inline const char* agencyName(const char* code) {
  struct A { const char* c; const char* n; };
  static const A kA[] = {
      {"kcm", "King County Metro"},  {"st", "Sound Transit"},    {"ct", "Community Transit"},
      {"pt", "Pierce Transit"},      {"et", "Everett Transit"},  {"it", "Intercity Transit"},
      {"kt", "Kitsap Transit"},      {"wsf", "WA State Ferries"}, {"mono", "Seattle Monorail"},
  };
  if (!code || !*code) return kA[0].n;
  for (const A& a : kA) {
    const char* x = a.c;
    const char* y = code;
    while (*x && *y && watchfeed::lower(*y) == *x) { x++; y++; }
    if (!*x && !*y) return a.n;
  }
  return code;
}

// ---- line matching ---------------------------------------------------------------------------
namespace detail {
// UTF-8 of the symbols BusBot writes.
#define TF_STOP "\xF0\x9F\x9A\x8F"     // 🚏
#define TF_COMPASS "\xF0\x9F\xA7\xAD"  // 🧭
#define TF_SEARCH "\xF0\x9F\x94\x8E"   // 🔎
#define TF_PIN "\xF0\x9F\x93\x8D"      // 📍
#define TF_WARN "\xE2\x9A\xA0"         // ⚠ (then often U+FE0F)
#define TF_OFF "\xF0\x9F\x93\xB4"      // 📴
#define TF_OK "\xE2\x9C\x85"           // ✅
#define TF_SLEEP "\xF0\x9F\x98\xB4"    // 😴
#define TF_WALK "\xF0\x9F\x9A\xB6"     // 🚶
#define TF_SCHED "\xF0\x9F\x95\x92"    // 🕒
#define TF_ARROW "\xE2\x86\x92"        // →
#define TF_DOT "\xC2\xB7"              // ·
#define TF_VS16 "\xEF\xB8\x8F"         // variation selector

struct Span {
  const char* s;
  size_t n;
};

inline bool starts(Span l, const char* w) {
  const size_t k = strlen(w);
  return l.n >= k && !memcmp(l.s, w, k);
}
inline Span skip(Span l, size_t k) {
  if (k > l.n) k = l.n;
  l.s += k;
  l.n -= k;
  if (l.n >= 3 && !memcmp(l.s, TF_VS16, 3)) { l.s += 3; l.n -= 3; }
  while (l.n && watchfeed::isSpace(*l.s)) { l.s++; l.n--; }
  return l;
}
inline Span trimEnd(Span l) {
  while (l.n && watchfeed::isSpace(l.s[l.n - 1])) l.n--;
  return l;
}
// Last occurrence of w in l, or -1.
inline long rfind(Span l, const char* w) {
  const size_t k = strlen(w);
  if (l.n < k) return -1;
  for (size_t i = l.n - k + 1; i-- > 0;)
    if (!memcmp(l.s + i, w, k)) return (long)i;
  return -1;
}
inline long find(Span l, const char* w, size_t from = 0) {
  const size_t k = strlen(w);
  for (size_t i = from; i + k <= l.n; i++)
    if (!memcmp(l.s + i, w, k)) return (long)i;
  return -1;
}
inline bool containsNoCase(Span l, const char* w) { return watchfeed::containsNoCase(l.s, l.n, w); }

// A route's mode emoji at the start of l; its length in *len.
inline uint8_t modeAt(Span l, size_t* len) {
  struct M { const char* e; uint8_t m; };
  static const M kM[] = {{"\xF0\x9F\x9A\x8C", kBus}, {"\xF0\x9F\x9A\x8D", kRapid}, {"\xF0\x9F\x9A\x8B", kRail},
                         {"\xF0\x9F\x9A\x86", kTrain}, {"\xE2\x9B\xB4", kFerry}};
  for (const M& m : kM)
    if (starts(l, m.e)) { *len = strlen(m.e); return m.m; }
  return kNoMode;
}

inline void put(char* out, size_t cap, Span s) { watchfeed::copyN(out, cap, s.s, s.n); }

// "now" / "5 mins" / "1 min" / "21:05"; minutes or hh:mm.
inline bool timeWord(Span t, int* minutes, int* hh, int* mm) {
  *minutes = -1; *hh = -1; *mm = -1;
  if (t.n == 3 && !memcmp(t.s, "now", 3)) { *minutes = 0; return true; }
  size_t d = 0;
  while (d < t.n && watchfeed::isDigit(t.s[d])) d++;
  if (!d) return false;
  if (d <= 2 && d < t.n && t.s[d] == ':' && t.n == d + 3 && watchfeed::isDigit(t.s[d + 1]) &&
      watchfeed::isDigit(t.s[d + 2])) {
    *hh = atoi(t.s);
    *mm = atoi(t.s + d + 1);
    return true;
  }
  Span r = {t.s + d, t.n - d};
  while (r.n && *r.s == ' ') { r.s++; r.n--; }
  if ((r.n == 3 && !memcmp(r.s, "min", 3)) || (r.n == 4 && !memcmp(r.s, "mins", 4))) {
    *minutes = atoi(t.s);
    return true;
  }
  return false;
}

// Strips a trailing " 🕒" or " (s)"; true when it had one.
inline bool scheduledMark(Span* t) {
  Span e = trimEnd(*t);
  if (e.n >= 4 && !memcmp(e.s + e.n - 4, TF_SCHED, 4)) { e.n -= 4; *t = trimEnd(e); return true; }
  if (e.n >= 3 && !memcmp(e.s + e.n - 3, "(s)", 3)) { e.n -= 3; *t = trimEnd(e); return true; }
  *t = e;
  return false;
}

// "<lat>,<lon> · <ref>" (after an optional 📍)
inline bool geoLine(Span l, double* lat, double* lon, char* ref, size_t cap) {
  if (starts(l, TF_PIN)) l = skip(l, 4);
  char buf[64];
  if (l.n >= sizeof buf) return false;
  memcpy(buf, l.s, l.n);
  buf[l.n] = 0;
  const char* e;
  double a, b;
  if (!watchfeed::coordAt(buf, &a, &b, &e)) return false;
  while (watchfeed::isSpace(*e)) e++;
  if (strncmp(e, TF_DOT, 2)) return false;
  e += 2;
  while (watchfeed::isSpace(*e)) e++;
  if (!*e || strchr(e, ' ') != strrchr(e, ' ')) return false;   // "1120" or "ct 1120"
  for (const char* p = e; *p; p++)
    if (!watchfeed::isAlnum(*p) && *p != ' ' && *p != '-' && *p != '_' && *p != '.') return false;
  if (!watchfeed::validPos(a, b)) return false;
  *lat = a;
  *lon = b;
  if (ref && cap) watchfeed::copyN(ref, cap, e, strlen(e));
  return true;
}
}  // namespace detail

// ---- the parsed reply ------------------------------------------------------------------------
struct Arrival {
  uint8_t mode = kNoMode;
  bool scheduled = false;
  int16_t minutes = -1;   // after it was sent; -1: a clock time instead
  int8_t hh = -1, mm = -1;
  char route[24] = {0};
};
struct Leg {
  bool walk = false;
  uint8_t mode = kNoMode;
  int16_t walk_min = 0;
  char route[12] = {0};
  char dep[6] = {0}, arr[6] = {0};   // "7:50", 12-hour clock
  char board[32] = {0};   // "Northgate Station" (older replies: a stop number)
  char to[36] = {0};
};
struct Hit {
  char ref[12] = {0};
  char name[40] = {0};
  char dist[10] = {0};
  char routes[28] = {0};
};

constexpr int kMaxArrivals = 6, kMaxAlerts = 3, kMaxLegs = 5, kMaxHits = 3;

struct Reply {
  uint8_t kind = kPart;
  bool offline = false, alerts_clear = false, alerts_unknown = false, no_trips = false;
  bool has_pos = false;
  double lat = 0, lon = 0;
  char title[48] = {0};
  char agency[6] = {0};    // "ct"; "" for the default agency
  char route[16] = {0};    // a route reply's route
  uint8_t route_mode = kNoMode;
  char stop_ref[12] = {0};
  char also[24] = {0};
  int n_arr = 0, n_alerts = 0, n_legs = 0, n_hits = 0;
  Arrival arr[kMaxArrivals];
  char alerts[kMaxAlerts][80] = {{0}};
  // a trip plan
  char plan_arrive[6] = {0};
  int16_t plan_min = -1;
  uint8_t plan_transfers = 0;
  Leg legs[kMaxLegs];
  Hit hits[kMaxHits];
};

// Reads [t] (one message, or a reply's joined messages). False when it isn't
// a BusBot reply: its first line must be a BusBot header, or (for the rest of
// a split reply) every line one BusBot writes, one of them unmistakable.
// [alert_posts]: a lone "⚠️ …" line is the bot's alert post (a transit channel).
inline bool parse(const char* t, Reply& r, bool alert_posts = false) {
  using namespace detail;
  r = Reply();
  bool first = true, strong = false;
  for (const char* line = t; line && *line;) {
    const char* le = strchr(line, '\n');
    Span l = {line, le ? (size_t)(le - line) : strlen(line)};
    line = le ? le + 1 : nullptr;
    while (l.n && watchfeed::isSpace(*l.s)) { l.s++; l.n--; }
    l = trimEnd(l);
    if (!l.n) continue;
    const bool was_first = first;
    first = false;
    size_t ml = 0;
    uint8_t mode;

    if (was_first) {
      // 🧭 Arrive 8:25 · 45 min · 1 transfer · timetable
      if (starts(l, TF_COMPASS)) {
        Span b = skip(l, 4);
        if (b.n > 7 && !memcmp(b.s, "Arrive ", 7) && containsNoCase(b, "timetable")) {
          r.kind = kPlan;
          snprintf(r.title, sizeof r.title, "Trip");
          Span c = skip(b, 7);
          size_t k = 0;
          while (k < c.n && c.s[k] != ' ') k++;
          put(r.plan_arrive, sizeof r.plan_arrive, {c.s, k});
          long d = find(c, TF_DOT);
          if (d >= 0) r.plan_min = (int16_t)atoi(c.s + d + 3);
          long x = find(c, "transfer");
          if (x > 2) {
            long q = x - 2;
            while (q > 0 && watchfeed::isDigit(c.s[q - 1])) q--;
            r.plan_transfers = (uint8_t)atoi(c.s + q);
          }
          continue;
        }
      }
      // 🚌 11 @ 3rd Ave & Pike St · CT
      if ((mode = modeAt(l, &ml)) != kNoMode) {
        Span b = skip(l, ml);
        long at = find(b, " @ ");
        if (at > 0) {
          r.kind = kRoute;
          r.route_mode = mode;
          put(r.route, sizeof r.route, {b.s, (size_t)at});
          Span st = {b.s + at + 3, b.n - (size_t)at - 3};
          long dot = rfind(st, " " TF_DOT " ");
          if (dot > 0 && st.n - (size_t)dot - 4 <= 4) {
            put(r.agency, sizeof r.agency, {st.s + dot + 4, st.n - (size_t)dot - 4});
            for (char* p = r.agency; *p; p++) *p = watchfeed::lower(*p);
            st.n = (size_t)dot;
          }
          put(r.title, sizeof r.title, st);
          continue;
        }
      }
      // 🚏 3rd Ave & Pike St, Bay 2 · CT   (not a search's "🚏 1120 name → routes")
      if (starts(l, TF_STOP) && find(l, TF_ARROW) < 0) {
        r.kind = kStop;
        Span st = skip(l, 4);
        long dot = rfind(st, " " TF_DOT " ");
        if (dot > 0 && st.n - (size_t)dot - 4 <= 4) {
          put(r.agency, sizeof r.agency, {st.s + dot + 4, st.n - (size_t)dot - 4});
          for (char* p = r.agency; *p; p++) *p = watchfeed::lower(*p);
          st.n = (size_t)dot;
        }
        long at = find(st, " @ ");   // "alerts [stop] [route]"
        if (at > 0) {
          put(r.route, sizeof r.route, {st.s, (size_t)at});
          st = {st.s + at + 3, st.n - (size_t)at - 3};
        }
        put(r.title, sizeof r.title, st);
        continue;
      }
      // 🔎 pike 3rd:   📍 Near you:
      if ((starts(l, TF_SEARCH) || starts(l, TF_PIN)) && l.s[l.n - 1] == ':') {
        r.kind = kSearch;
        Span st = skip(l, 4);
        st.n--;
        put(r.title, sizeof r.title, st);
        continue;
      }
      // ⚠️ CT 201: Detour …  (the bot's alert post, alone in its message)
      if (alert_posts && starts(l, TF_WARN) && (!line || !*line)) {
        r.kind = kAlertPost;
        Span b = skip(l, 3);
        long c = find(b, ": ");
        if (c > 0 && c <= 40) {
          put(r.title, sizeof r.title, {b.s, (size_t)c});
          b = {b.s + c + 2, b.n - (size_t)c - 2};
        } else {
          snprintf(r.title, sizeof r.title, "Service alert");
        }
        put(r.alerts[0], sizeof r.alerts[0], b);
        r.n_alerts = 1;
        continue;
      }
    }

    // ---- body lines ----
    double la, lo;
    if (geoLine(l, &la, &lo, r.stop_ref, sizeof r.stop_ref)) {
      r.has_pos = true; r.lat = la; r.lon = lo; strong = true;
      continue;
    }
    if (starts(l, TF_OFF)) {
      if (containsNoCase(l, "offline")) r.offline = true;
      else if (containsNoCase(l, "alerts")) r.alerts_unknown = true;
      else if (r.kind == kPart) return false;
      continue;
    }
    if (starts(l, TF_OK) && containsNoCase(l, "no alerts")) { r.alerts_clear = true; continue; }
    if (starts(l, TF_SLEEP) && containsNoCase(l, "no upcoming")) { r.no_trips = true; continue; }
    if (l.n > 5 && !memcmp(l.s, "also:", 5)) {
      put(r.also, sizeof r.also, skip(l, 5));
      strong = true;
      continue;
    }
    if (starts(l, TF_WARN)) {
      if (r.n_alerts < kMaxAlerts) put(r.alerts[r.n_alerts++], sizeof r.alerts[0], skip(l, 3));
      continue;
    }
    // 🚶 1 min to 1121
    if (starts(l, TF_WALK)) {
      Span b = skip(l, 4);
      long to = find(b, " min to ");
      if (to > 0 && r.n_legs < kMaxLegs) {
        Leg& g = r.legs[r.n_legs++];
        g.walk = true;
        g.walk_min = (int16_t)atoi(b.s);
        put(g.to, sizeof g.to, {b.s + to + 8, b.n - (size_t)to - 8});
        continue;
      }
    }
    // 🚌 11 7:50 11040 → 3rd Ave & Pike St, Bay 2 8:00
    if ((mode = modeAt(l, &ml)) != kNoMode && find(l, TF_ARROW) > 0) {
      Span b = skip(l, ml);
      long ar = find(b, " " TF_ARROW " ");
      if (ar > 0 && r.n_legs < kMaxLegs) {
        Leg& g = r.legs[r.n_legs++];
        g.mode = mode;
        Span head = {b.s, (size_t)ar};
        Span tail = {b.s + ar + 5, b.n - (size_t)ar - 5};
        // tail: "<to> <h:mm>"
        long sp = rfind(tail, " ");
        if (sp > 0) {
          put(g.arr, sizeof g.arr, {tail.s + sp + 1, tail.n - (size_t)sp - 1});
          tail.n = (size_t)sp;
        }
        put(g.to, sizeof g.to, tail);
        // head: "<route> <h:mm>[ <board>]": the clock is the last word with a ':'
        long colon = rfind(head, ":");
        if (colon > 0) {
          long ts = colon;
          while (ts > 0 && head.s[ts - 1] != ' ') ts--;
          long te = colon;
          while ((size_t)te < head.n && head.s[te] != ' ') te++;
          put(g.dep, sizeof g.dep, {head.s + ts, (size_t)(te - ts)});
          if ((size_t)te < head.n) {
            Span b = skip({head.s + te, head.n - (size_t)te}, 0);
            if (b.n > 5 && !memcmp(b.s, "from ", 5)) b = skip(b, 5);
            put(g.board, sizeof g.board, b);
          }
          if (ts > 0) put(g.route, sizeof g.route, trimEnd({head.s, (size_t)ts}));
        }
        strong = true;
        continue;
      }
    }
    // 🚏 11050 Broadway & E Madison St · 0.6 mi → 49
    if (starts(l, TF_STOP)) {
      long ar = find(l, " " TF_ARROW " ");
      if (ar > 0 && r.n_hits < kMaxHits) {
        Hit& h = r.hits[r.n_hits++];
        Span b = skip({l.s, (size_t)ar}, 4);
        put(h.routes, sizeof h.routes, {l.s + ar + 5, l.n - (size_t)ar - 5});
        long dot = rfind(b, " " TF_DOT " ");
        if (dot > 0) {
          put(h.dist, sizeof h.dist, {b.s + dot + 4, b.n - (size_t)dot - 4});
          b.n = (size_t)dot;
        }
        // ref: the first word, or "ct 1120"
        size_t k = 0;
        while (k < b.n && b.s[k] != ' ') k++;
        if (k <= 4 && k < b.n && !watchfeed::isDigit(b.s[0])) {
          k++;
          while (k < b.n && b.s[k] != ' ') k++;
        }
        put(h.ref, sizeof h.ref, {b.s, k});
        put(h.name, sizeof h.name, skip({b.s + k, b.n - k}, 0));
        strong = true;
        continue;
      }
    }
    // Arrivals: "🚌 11 - 5 mins 🕒" (a stop), "5 mins 🕒" (a route reply)
    {
      Span b = l;
      mode = modeAt(b, &ml);
      if (mode != kNoMode) b = skip(b, ml);
      const bool sched = scheduledMark(&b);
      int mins, hh, mm;
      long dash = rfind(b, " - ");
      Span route = {b.s, 0}, tw = b;
      if (dash > 0) { route.n = (size_t)dash; tw = {b.s + dash + 3, b.n - (size_t)dash - 3}; }
      const bool ok = timeWord(tw, &mins, &hh, &mm) && (dash > 0 || (r.kind == kRoute && mode == kNoMode));
      if (ok) {
        if (r.n_arr < kMaxArrivals) {
          Arrival& a = r.arr[r.n_arr++];
          a.mode = mode != kNoMode ? mode : r.route_mode;
          a.scheduled = sched;
          a.minutes = (int16_t)mins;
          a.hh = (int8_t)hh;
          a.mm = (int8_t)mm;
          if (dash > 0) put(a.route, sizeof a.route, route);
          else watchfeed::copyN(a.route, sizeof a.route, r.route, strlen(r.route));
        }
        if (mode != kNoMode) strong = true;
        continue;
      }
    }
    if (r.kind == kPart) return false;   // the rest of a reply is only BusBot lines
  }
  if (r.kind == kPart && !strong) return false;
  if (r.kind == kStop && !r.n_arr && !r.no_trips && (r.n_alerts || r.alerts_clear || r.alerts_unknown))
    r.kind = kAlerts;
  return true;
}

// True when [body]'s first line is one of BusBot's headers (not the rest of
// a split reply).
inline bool hasHeader(const char* body, bool alert_posts = false) {
  Reply r;
  return parse(body, r, alert_posts) && r.kind != kPart;
}

// A channel named for transit ("#transit", "#seatransit"): the bot's lone
// alert posts count there.
inline bool transitChannel(const char* name) {
  return name && watchfeed::containsNoCase(name, strlen(name), "transit");
}

// "1121", "ct 1120", "st S_EV": a stop number rather than a stop's name.
inline bool isStopNumber(const char* s) {
  if (!s || !*s) return false;
  const char* sp = strchr(s, ' ');
  if (sp && (sp - s > 4 || strchr(sp + 1, ' '))) return false;
  bool digit = false;
  for (const char* p = sp ? sp + 1 : s; *p; p++) {
    if (watchfeed::isDigit(*p)) digit = true;
    else if (!watchfeed::isAlnum(*p) && *p != '_' && *p != '-' && *p != '.') return false;
  }
  return digit;
}

// ---- for the Watch list, map and sheet --------------------------------------------------------

inline const char* statusWord(const Reply& r) {
  switch (r.kind) {
    case kPlan: return "TRIP";
    case kSearch: return "STOPS";
    case kAlerts: return "ALERTS";
    case kAlertPost: return "ALERT";
    default: break;
  }
  const uint8_t m = r.route_mode ? r.route_mode : (r.n_arr ? r.arr[0].mode : (uint8_t)kNoMode);
  return m == kFerry ? "FERRY" : m == kTrain ? "TRAIN" : m == kRail ? "RAIL" : "BUS";
}

// The card's title: "3rd Ave & Pike St", "11 @ 3rd Ave…", "Trip to 11050".
inline void headline(const Reply& r, char* out, size_t cap) {
  if (r.kind == kPlan) {
    const char* to = r.n_legs ? r.legs[r.n_legs - 1].to : "";
    snprintf(out, cap, "Arrive %s%s%s", r.plan_arrive, *to ? " " TF_DOT " " : "", to);
  } else if (r.route[0]) {
    snprintf(out, cap, "%s @ %s", r.route, r.title[0] ? r.title : r.stop_ref);
  } else {
    snprintf(out, cap, "%s", r.title[0] ? r.title : (r.stop_ref[0] ? r.stop_ref : "Transit"));
  }
}

// The level colour: alerts to read are yellow, offline grey, else blue.
inline uint8_t level(const Reply& r) {
  if (r.kind == kAlertPost || r.n_alerts) return watchfeed::kActive;
  if (r.offline) return watchfeed::kNone;
  return watchfeed::kOther;
}

// "4 min", "due", "gone" for [secs] from now.
inline void countdown(long secs, char* out, size_t cap) {
  if (secs < -120) snprintf(out, cap, "gone");
  else if (secs < 60) snprintf(out, cap, "due");
  else if (secs < 3600) snprintf(out, cap, "%ld min", secs / 60);
  else snprintf(out, cap, "%ld h %02ld", secs / 3600, (secs % 3600) / 60);
}

// When an arrival is, in seconds after the reply was sent; false for a
// clock time with no clock to place it.
inline bool arrivalOffset(const Arrival& a, uint32_t sent_epoch, long* secs) {
  if (a.minutes >= 0) { *secs = (long)a.minutes * 60; return true; }
  if (a.hh < 0 || sent_epoch < 1700000000UL) return false;
  time_t s = (time_t)sent_epoch;
  struct tm tm;
  if (!localtime_r(&s, &tm)) return false;
  tm.tm_hour = a.hh;
  tm.tm_min = a.mm;
  tm.tm_sec = 0;
  time_t at = mktime(&tm);
  if (at < s - 3600) at += 86400;
  *secs = (long)(at - s);
  return true;
}

// A plan's 12-hour "7:50" as seconds after [sent_epoch].
inline bool planOffset(const char* clock, uint32_t sent_epoch, long* secs) {
  if (!clock || !*clock || sent_epoch < 1700000000UL) return false;
  const int h = atoi(clock);
  const char* c = strchr(clock, ':');
  if (!c) return false;
  const int m = atoi(c + 1);
  const time_t s = (time_t)sent_epoch;
  for (int d = 0; d < 2; d++) {
    for (int k = 0; k < 2; k++) {
      const int hh = h % 12 + 12 * k;
      struct tm tm;
      if (!localtime_r(&s, &tm)) return false;
      tm.tm_mday += d;
      tm.tm_hour = hh;
      tm.tm_min = m;
      tm.tm_sec = 0;
      const time_t at = mktime(&tm);
      if (at >= s - 3600) { *secs = (long)(at - s); return true; }
    }
  }
  return false;
}

// "8:14" in the radio's local time, [secs] after [sent_epoch]; "" without a clock.
inline void clockAt(uint32_t sent_epoch, long secs, char* out, size_t cap) {
  out[0] = 0;
  if (sent_epoch < 1700000000UL) return;
  const time_t t = (time_t)sent_epoch + secs;
  struct tm tm;
  if (localtime_r(&t, &tm)) snprintf(out, cap, "%d:%02d", tm.tm_hour, tm.tm_min);
}

namespace detail {
struct Out {
  char* s;
  size_t cap, n;
  Out(char* out, size_t c) : s(out), cap(c), n(0) {}
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(s + n, cap - n, fmt, ap);
    va_end(ap);
    if (k > 0) n = (size_t)k < cap - n ? n + (size_t)k : cap - 1;
  }
  void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (n && n + 1 < cap) s[n++] = '\n', s[n] = 0;
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(s + n, cap - n, fmt, ap);
    va_end(ap);
    if (k > 0) n = (size_t)k < cap - n ? n + (size_t)k : cap - 1;
  }
};
}  // namespace detail

// The card's text: one line per arrival, alert, step or stop, countdowns
// [elapsed_s] after the reply was sent ([sent_epoch] 0 when the radio has no
// clock: then clock times are left out). [full]: everything (the sheet);
// otherwise the first few lines (the Watch list).
inline size_t describe(const Reply& r, uint32_t elapsed_s, uint32_t sent_epoch, bool full, char* out, size_t cap) {
  if (!cap) return 0;
  out[0] = 0;
  detail::Out o(out, cap);
  char cd[16], ck[8];
  if (full && r.kind != kPlan && r.kind != kSearch && r.kind != kAlertPost) {
    char ag[8] = "";
    if (!r.agency[0] && r.stop_ref[0] && strchr(r.stop_ref, ' '))
      watchfeed::copyN(ag, sizeof ag, r.stop_ref, (size_t)(strchr(r.stop_ref, ' ') - r.stop_ref));
    o.line("%s%s%s%s%s", r.stop_ref[0] ? "Stop " : "", r.stop_ref, r.stop_ref[0] ? " " TF_DOT " " : "",
           agencyName(r.agency[0] ? r.agency : ag), r.route_mode ? "" : "");
  }
  if (r.offline) o.line("Offline: timetable times");
  const int n_arr = full ? r.n_arr : (r.n_arr < 3 ? r.n_arr : 3);
  for (int i = 0; i < n_arr; i++) {
    const Arrival& a = r.arr[i];
    long secs;
    cd[0] = 0;
    ck[0] = 0;
    if (arrivalOffset(a, sent_epoch, &secs)) {
      countdown(secs - (long)elapsed_s, cd, sizeof cd);
      clockAt(sent_epoch, secs, ck, sizeof ck);
    }
    const char* mn = modeName(a.mode);
    if (full)
      o.line("%s%s%s  %s%s%s  %s", *mn ? mn : "", *mn ? " " : "", a.route, cd, ck[0] ? " " TF_DOT " " : "", ck,
             a.scheduled ? "timetable" : "live");
    else
      o.line("%s  %s%s%s%s", a.route, cd, ck[0] ? " (" : "", ck, ck[0] ? ")" : "");
  }
  if (!full && r.n_arr > n_arr) o.line("+%d more", r.n_arr - n_arr);
  if (r.no_trips) o.line("No more trips today");
  for (int i = 0; i < r.n_alerts && (full || i < 2); i++) o.line("! %s", r.alerts[i]);
  if (r.alerts_clear) o.line("No alerts");
  if (r.alerts_unknown) o.line("Live alerts couldn't be checked");
  if (r.kind == kPlan) {
    long secs;
    cd[0] = 0;
    if (planOffset(r.plan_arrive, sent_epoch, &secs) && secs > (long)elapsed_s) {
      countdown(secs - (long)elapsed_s, cd, sizeof cd);
    }
    o.line("Arrive %s%s%s%s", r.plan_arrive, cd[0] ? " (in " : "", cd, cd[0] ? ")" : "");
    if (r.plan_min >= 0) o.add(" " TF_DOT " %d min", r.plan_min);
    if (r.plan_transfers) o.add(" " TF_DOT " %u transfer%s", r.plan_transfers, r.plan_transfers > 1 ? "s" : "");
    o.add(" " TF_DOT " timetable");
  }
  for (int i = 0; i < r.n_legs && (full || i < 2); i++) {
    const Leg& g = r.legs[i];
    if (g.walk) {
      if (!strcmp(g.to, "destination")) o.line("Walk %d min to your destination", g.walk_min);
      else o.line("Walk %d min to %s%s", g.walk_min, isStopNumber(g.to) ? "stop " : "", g.to);
      continue;
    }
    long secs;
    cd[0] = 0;
    if (planOffset(g.dep, sent_epoch, &secs) && secs > (long)elapsed_s) countdown(secs - (long)elapsed_s, cd, sizeof cd);
    o.line("%s %s %s%s%s%s%s%s", modeName(g.mode), g.route, g.dep, g.board[0] ? " from " : "",
           isStopNumber(g.board) ? "stop " : "", g.board,
           cd[0] ? ", leaves in " : "", cd);
    o.line("  to %s%s%s", g.to, g.arr[0] ? ", " : "", g.arr);
  }
  if (!full && r.n_legs > 2) o.line("+%d more steps", r.n_legs - 2);
  for (int i = 0; i < r.n_hits && (full || i < 2); i++) {
    const Hit& h = r.hits[i];
    o.line("%s %s%s%s", h.ref, h.name, h.dist[0] ? " " TF_DOT " " : "", h.dist);
    if (full && h.routes[0]) o.line("  routes %s", h.routes);
  }
  if (full && r.also[0]) o.line("Same stop number at: %s", r.also);
  if (full && r.n_arr) {
    ck[0] = 0;
    clockAt(sent_epoch, 0, ck, sizeof ck);
    if (ck[0]) o.line("Times from %s, counting down", ck);
  }
  return o.n;
}

// A Watch report for the map and list: position, title, level, and the
// first lines of the card as its body.
inline bool toReport(const Reply& r, watchfeed::Report& w) {
  if (!r.has_pos) return false;
  w = watchfeed::Report();
  w.lat = r.lat;
  w.lon = r.lon;
  w.level = level(r);
  headline(r, w.title, sizeof w.title);
  describe(r, 0, 0, false, w.body, sizeof w.body);
  for (char* p = w.body; *p; p++)
    if (*p == '\n') *p = ';';
  return true;
}

}  // namespace transitfeed
