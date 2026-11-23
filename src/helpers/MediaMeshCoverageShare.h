// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sending MediaMesh coverage points to an analyzer (Settings > MediaMesh >
// Share coverage points; MEDIAMESH.md, "Sharing coverage points"), the same
// as MeshCore Open TAK (lib/services/mediamesh_coverage_share.dart).
//
// The points come from /meshcomod/mediamesh_coverage.csv, a line per
// repeater heard (or one with blanks when none answered); lines of one sample
// share epoch, lat and lon. A batch is a CoreScope (CascadiaMesh fork)
// POST /api/mediamesh/coverage-points body, signed by the standalone's own
// key over kSignPrefix + the hex SHA-256 of the body. Lines from before the
// repeaters' keys were written (no key column) can't be sent: a sample with
// one is left out whole, so it doesn't read as "nobody answered".
//
// No Arduino here: test/test_mediamesh_coverage_share.cpp runs it on the host.

#pragma once

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mmshare {

static constexpr const char* kDefaultAnalyzer = "https://analyzer.cascadiamesh.org/api/mediamesh/coverage-points";
static constexpr const char* kUploadPath = "/api/mediamesh/coverage-points";
static constexpr const char* kSignPrefix = "corescope-coverage:";
static constexpr const char* kDeletePrefix = "corescope-coverage-delete:";
static constexpr int kBatchMax = 200;          // samples a batch
static constexpr size_t kSampleRoom = 900;     // bytes a sample can take (6 repeaters, RSSI, those asked)
static constexpr int kSilentMax = 6;           // repeaters asked by name that didn't answer, a sample

/** The upload address for what was typed as the analyzer: the full address the
 *  points go to, kept as it is, or just the analyzer ("analyzer.example.org",
 *  "https://host/path/"), which gets kUploadPath added. false when it isn't a
 *  web address. */
inline bool endpointFor(const char* analyzer, char* out, size_t cap) {
  if (!analyzer || !out || cap < 16) return false;
  while (*analyzer == ' ') analyzer++;
  char a[160];
  snprintf(a, sizeof a, "%s", analyzer);
  size_t n = strlen(a);
  while (n && (a[n - 1] == ' ' || a[n - 1] == '/')) a[--n] = 0;
  if (!n) return false;
  const char* scheme = "https://";
  const char* rest = a;
  if (!strncmp(a, "https://", 8)) rest = a + 8;
  else if (!strncmp(a, "http://", 7)) { scheme = "http://"; rest = a + 7; }
  else if (strstr(a, "://")) return false;
  // a host: letters, digits, dots, dashes, a port; then an optional path
  size_t h = 0;
  while (rest[h] && rest[h] != '/' && rest[h] != '?' && rest[h] != '#') {
    const char ch = rest[h];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == ':'))
      return false;
    h++;
  }
  if (!h) return false;
  size_t p = h;
  while (rest[p] && rest[p] != '?' && rest[p] != '#') p++;
  while (p > h && rest[p - 1] == '/') p--;
  const size_t up = strlen(kUploadPath);
  const bool has = p >= h + up && !strncmp(rest + p - up, kUploadPath, up);
  const int w = snprintf(out, cap, "%s%.*s%s", scheme, (int)p, rest, has ? "" : kUploadPath);
  return w > 0 && (size_t)w < cap;
}

/** One line of the coverage file. */
struct Row {
  uint32_t epoch = 0;
  double lat = 0, lon = 0;
  bool none = true;      // no repeater answered (the line's repeater fields are blank)
  bool has_key = false;  // the repeater's key was written (newer lines)
  char key[9] = "";
  float snr = 0, remote = 0, margin = 0;
  int rssi = 0;          // of a discovery answer, dBm; 0: not known
  bool radio2 = false;   // it answered being asked by name on radio2 (its margin is radio2's own)
  char silent[kSilentMax][9] = {};   // asked by name on radio2 and silent (on a sample's first line)
  int silent_n = 0;
};

/** Up to 8 hex digits at p, lower-cased, into out; the count read. */
inline size_t hexKey(const char* p, char out[9]) {
  size_t k = 0;
  while (k < 8 && p[k] && p[k] != '\n' && p[k] != '\r' && p[k] != ',' && p[k] != ';') {
    char c = p[k];
    if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) break;
    out[k++] = c;
  }
  out[k] = 0;
  return k;
}

/** epoch,lat,lon,reach,best_margin,repeater,snr,remote_snr,margin[,key[,rssi,via,silent]]:
 *  false for the header, a blank or a broken line. */
inline bool parseRow(const char* line, Row& r) {
  r = Row();
  if (!line || *line < '0' || *line > '9') return false;
  const char* f[14];
  int n = 0;
  f[n++] = line;
  for (const char* q = line; *q && *q != '\n' && *q != '\r' && n < 14; q++)
    if (*q == ',') f[n++] = q + 1;
  if (n < 9) return false;
  char* end = nullptr;
  r.epoch = (uint32_t)strtoul(f[0], &end, 10);
  if (end != f[1] - 1 || !r.epoch) return false;
  r.lat = strtod(f[1], &end);
  if (end != f[2] - 1) return false;
  r.lon = strtod(f[2], &end);
  if (end != f[3] - 1) return false;
  if (r.lat == 0 && r.lon == 0) return false;
  r.none = !strncmp(f[3], "none", 4);
  if (n >= 13) {   // the repeaters asked by name that didn't answer, ';' between
    for (const char* p = f[12]; *p && *p != '\n' && *p != '\r' && r.silent_n < kSilentMax;) {
      char k[9];
      const size_t len = hexKey(p, k);
      if (len == 8) snprintf(r.silent[r.silent_n++], 9, "%s", k);
      while (*p && *p != ';' && *p != '\n' && *p != '\r') p++;
      if (*p == ';') p++;
    }
  }
  if (r.none) return true;
  if (n >= 11 && f[10][0] != ',' && f[10][0] != '\n') {
    const long v = strtol(f[10], nullptr, 10);
    if (v < 0 && v >= -150) r.rssi = (int)v;
  }
  if (n >= 12) r.radio2 = !strncmp(f[11], "radio2", 6);
  r.snr = strtof(f[6], nullptr);
  r.remote = strtof(f[7], nullptr);
  r.margin = strtof(f[8], nullptr);
  if (n >= 10) r.has_key = hexKey(f[9], r.key) == 8;
  return true;
}

/** Metres between two points (equirectangular: plenty between samples). */
inline double metres(double lat1, double lon1, double lat2, double lon2) {
  const double k = 3.14159265358979323846 / 180.0;
  const double x = (lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * k), y = (lat2 - lat1) * k;
  return sqrt(x * x + y * y) * 6371000.0;
}

/** Builds one batch's body into buf, row by row. */
class Batch {
 public:
  Batch(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

  bool begin(const char* radio_hex, uint32_t sent, const char* app, uint32_t main_bw_hz, uint32_t r2_bw_hz, uint8_t r2_sf) {
    len_ = 0;
    samples_ = 0;
    open_ = skip_ = false;
    have_prev_ = false;
    last_epoch_ = 0;
    put("{\"v\":1,\"radio\":\"%s\",\"sent\":%lu,\"app\":\"%s\",\"radio2\":{\"bw_hz\":%lu,\"sf\":%u},\"main_bw_hz\":%lu,\"points\":[",
        radio_hex, (unsigned long)sent, app, (unsigned long)r2_bw_hz, (unsigned)r2_sf, (unsigned long)main_bw_hz);
    return !over_;
  }

  /** Adds a row. false: the batch is full and the row was not taken (it starts
   *  the next batch); rows of one sample are always taken together. */
  bool add(const Row& r) {
    const bool same = open_ || skip_ ? (r.epoch == cur_.epoch && r.lat == cur_.lat && r.lon == cur_.lon) : false;
    if (!same) {
      closeSample();
      if (samples_ >= kBatchMax || len_ + kSampleRoom + 4 > cap_) return false;
      startSample(r);
    }
    if (skip_) return true;
    for (int i = 0; i < r.silent_n; i++) {   // asked by name, silent: once each
      bool had = false;
      for (int j = 0; j < silent_n_ && !had; j++) had = !strcmp(silent_[j], r.silent[i]);
      if (!had && silent_n_ < kSilentMax) snprintf(silent_[silent_n_++], 9, "%s", r.silent[i]);
    }
    if (r.none) return true;
    if (!r.has_key) {   // a line from before keys were written: the whole sample goes
      len_ = sample_at_;
      open_ = false;
      skip_ = true;
      return true;
    }
    put("%s{\"key\":\"%s\",\"snr\":%.2f,\"remote_snr\":%.2f,\"margin\":%.1f", reps_ ? "," : "", r.key, r.snr, r.remote, r.margin);
    if (r.rssi) put(",\"rssi\":%d", r.rssi);
    if (r.radio2) put(",\"via\":\"radio2\"");
    put("}");
    reps_++;
    return true;
  }

  /** Closes the body; false when it overflowed. */
  bool finish() {
    closeSample();
    put("]}");
    return !over_;
  }

  int samples() const { return samples_; }
  size_t len() const { return len_; }
  /** The last sample taken or left out: everything up to it is done. */
  uint32_t lastEpoch() const { return last_epoch_; }

 private:
  void put(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (over_) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf_ + len_, cap_ - len_, fmt, ap);
    va_end(ap);
    if (w < 0 || len_ + (size_t)w >= cap_) { over_ = true; return; }
    len_ += (size_t)w;
  }
  void startSample(const Row& r) {
    // how fast it was going since the sample before (a walk or a drive)
    float speed = 0;
    if (have_prev_ && r.epoch > prev_epoch_ && r.epoch - prev_epoch_ <= 300)
      speed = (float)(metres(prev_lat_, prev_lon_, r.lat, r.lon) / (r.epoch - prev_epoch_));
    have_prev_ = true;
    prev_epoch_ = r.epoch;
    prev_lat_ = r.lat;
    prev_lon_ = r.lon;
    cur_ = r;
    skip_ = false;
    reps_ = 0;
    silent_n_ = 0;
    sample_at_ = len_;
    put("%s{\"t\":%lu,\"lat\":%.6f,\"lon\":%.6f,", samples_ ? "," : "", (unsigned long)r.epoch, r.lat, r.lon);
    if (speed > 0) put("\"speed\":%.1f,", speed);
    put("\"reps\":[");
    open_ = true;
  }
  void closeSample() {
    if (open_) {
      put("]");
      if (silent_n_) {
        put(",\"silent\":[");
        for (int i = 0; i < silent_n_; i++) put("%s\"%s\"", i ? "," : "", silent_[i]);
        put("]");
      }
      put("}");
      samples_++;
      last_epoch_ = cur_.epoch;
    } else if (skip_) {
      last_epoch_ = cur_.epoch;
    }
    open_ = skip_ = false;
  }

  char* buf_;
  size_t cap_, len_ = 0, sample_at_ = 0;
  int samples_ = 0, reps_ = 0, silent_n_ = 0;
  char silent_[kSilentMax][9] = {};
  bool open_ = false, skip_ = false, over_ = false, have_prev_ = false;
  Row cur_;
  uint32_t last_epoch_ = 0, prev_epoch_ = 0;
  double prev_lat_ = 0, prev_lon_ = 0;
};

/** The delete request's body. */
inline int deleteBody(char* out, size_t cap, const char* radio_hex, uint32_t sent) {
  return snprintf(out, cap, "{\"radio\":\"%s\",\"sent\":%lu}", radio_hex, (unsigned long)sent);
}

/** What the radio signs: prefix + the hex of a 32-byte SHA-256. */
inline int signedMessage(char* out, size_t cap, const char* prefix, const uint8_t digest[32]) {
  int o = snprintf(out, cap, "%s", prefix);
  for (int i = 0; i < 32 && o > 0 && (size_t)o + 3 <= cap; i++) o += snprintf(out + o, cap - o, "%02x", digest[i]);
  return o;
}

// ---- Delivery receipts (MEDIAMESH.md, "Sharing delivery receipts") ----
// For each photo or voice clip the standalone fetched whole, or gave up on
// after asking for it: the clip's session id (on the air in every piece),
// kind, piece counts and whether it ended up whole. Never its content, who
// sent it, the channel or where the standalone was. CoreScope's POST
// /api/mediamesh/receipts, signed over kReceiptSignPrefix + the body's
// SHA-256 hex, like coverage points.

static constexpr const char* kReceiptSignPrefix = "corescope-receipt:";
static constexpr const char* kReceiptDeletePrefix = "corescope-receipt-delete:";
static constexpr const char* kReceiptsPath = "/api/mediamesh/receipts";

struct Receipt {
  uint32_t sid = 0;
  bool voice = false;   // a voice clip; else a photo
  uint32_t at = 0;      // epoch: when it finished
  uint16_t total = 0, got = 0, asked = 0, rebuilt = 0;
  bool whole = false;
};

/** Where receipts go, from the analyzer the coverage points go to. */
inline bool receiptsEndpointFor(const char* analyzer, char* out, size_t cap) {
  char ep[200];
  if (!endpointFor(analyzer, ep, sizeof ep)) return false;
  const size_t n = strlen(ep), up = strlen(kUploadPath);
  if (n < up || strcmp(ep + n - up, kUploadPath) != 0) return false;   // endpointFor always ends with it
  const int w = snprintf(out, cap, "%.*s%s", (int)(n - up), ep, kReceiptsPath);
  return w > 0 && (size_t)w < cap;
}

// Appends to out at *len; *len = cap once anything doesn't fit.
inline void putf(char* out, size_t cap, size_t* len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
inline void putf(char* out, size_t cap, size_t* len, const char* fmt, ...) {
  if (*len >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(out + *len, cap - *len, fmt, ap);
  va_end(ap);
  *len = (w < 0 || *len + (size_t)w >= cap) ? cap : *len + (size_t)w;
}

/** A receipts batch body; its length, or 0 when it doesn't fit. */
inline size_t receiptsBody(char* out, size_t cap, const char* radio_hex, uint32_t sent, const char* app,
                           const Receipt* r, int n) {
  size_t len = 0;
  putf(out, cap, &len, "{\"v\":1,\"radio\":\"%s\",\"sent\":%lu,\"app\":\"%s\",\"receipts\":[", radio_hex,
       (unsigned long)sent, app);
  for (int i = 0; i < n; i++)
    putf(out, cap, &len,
         "%s{\"sid\":\"%08lx\",\"kind\":\"%s\",\"at\":%lu,\"total\":%u,\"got\":%u,\"asked\":%u,\"rebuilt\":%u,\"whole\":%s}",
         i ? "," : "", (unsigned long)r[i].sid, r[i].voice ? "voice" : "photo", (unsigned long)r[i].at,
         (unsigned)r[i].total, (unsigned)r[i].got, (unsigned)r[i].asked, (unsigned)r[i].rebuilt,
         r[i].whole ? "true" : "false");
  putf(out, cap, &len, "]}");
  return len >= cap ? 0 : len;
}

/** The analyzer's "error" from a reply body, into out ("" if none). */
inline void replyError(const char* body, char* out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  const char* e = body ? strstr(body, "\"error\":\"") : nullptr;
  if (!e) return;
  e += 9;
  size_t n = 0;
  while (e[n] && e[n] != '"' && n + 1 < cap) n++;
  snprintf(out, cap, "%.*s", (int)n, e);
}

}  // namespace mmshare
