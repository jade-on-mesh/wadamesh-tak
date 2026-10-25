// SPDX-License-Identifier: GPL-3.0-or-later
//
// u-blox M10 start-up aiding (T-Deck Plus, T-Deck Pro/Max, T-LoRa Pager:
// MIA-M10Q), sent once each time the receiver comes up. Adapted from MatterMesh
// OS (tdeck-plus/src/services/gps_assist.h, GPL-3.0, see NOTICE):
//
//  - AssistNow Autonomous (CFG-ANA-USE_ANA, RAM + battery-backed RAM): the
//    receiver predicts satellite orbits itself from the ones it has seen, for
//    days, so a start after hours off still has usable ephemerides.
//  - MGA-INI-TIME_UTC: the time from our clock, so it knows which satellites
//    are up without decoding the time from the sky first.
//  - MGA-INI-POS_LLH: the last known position (to 100 km: the device may have
//    moved), which narrows the search.
//
// RAM and battery-backed RAM only: nothing is written to the module's flash.
// Receivers that don't speak UBX ignore the frames. Pure (no Arduino): host-
// tested by test/test_ubx_assist.cpp.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace ubxassist {

static constexpr uint32_t kMinEpoch = 1704067200u;   // 2024-01-01: a clock older than this isn't set
static constexpr uint32_t kMaxEpoch = 4102444800u;   // 2100-01-01
static constexpr uint16_t kTimeAccuracyS = 30;
static constexpr uint32_t kPositionAccuracyCm = 10000000u;   // 100 km

struct Frame {
  uint8_t bytes[40] = {};
  size_t size = 0;   // 0: nothing to send
};

inline Frame frame(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  Frame f;
  if (len + 8u > sizeof f.bytes) return f;
  uint8_t* b = f.bytes;
  b[0] = 0xB5; b[1] = 0x62; b[2] = cls; b[3] = id;
  b[4] = (uint8_t)len; b[5] = (uint8_t)(len >> 8);
  for (uint16_t i = 0; i < len; i++) b[6 + i] = payload[i];
  uint8_t ck_a = 0, ck_b = 0;
  for (unsigned i = 2; i < 6u + len; i++) { ck_a += b[i]; ck_b += ck_a; }
  b[6 + len] = ck_a; b[7 + len] = ck_b;
  f.size = 8u + len;
  return f;
}

inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// CFG-VALSET v0, layers RAM | BBR, CFG-ANA-USE_ANA (0x10230001) = 1.
inline Frame enableAutonomous() {
  const uint8_t p[] = {0x00, 0x03, 0x00, 0x00, 0x01, 0x00, 0x23, 0x10, 0x01};
  return frame(0x06, 0x8A, p, sizeof p);
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
inline void civil(uint32_t days, int* year, unsigned* month, unsigned* day) {
  const int64_t z = (int64_t)days + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
  *year = (int)yoe + (int)era * 400 + (*month <= 2);
}

// MGA-INI-TIME_UTC: no external time reference, leap seconds unknown (-128).
inline Frame timeUtc(uint32_t epoch, uint16_t accuracy_s = kTimeAccuracyS) {
  if (epoch < kMinEpoch || epoch >= kMaxEpoch) return Frame();
  int year; unsigned month, day;
  civil(epoch / 86400u, &year, &month, &day);
  const uint32_t sec = epoch % 86400u;
  uint8_t p[24] = {0x10, 0x00, 0x00, 0x80};
  p[4] = (uint8_t)year; p[5] = (uint8_t)(year >> 8);
  p[6] = (uint8_t)month; p[7] = (uint8_t)day;
  p[8] = (uint8_t)(sec / 3600); p[9] = (uint8_t)(sec / 60 % 60); p[10] = (uint8_t)(sec % 60);
  p[16] = (uint8_t)accuracy_s; p[17] = (uint8_t)(accuracy_s >> 8);
  return frame(0x13, 0x40, p, sizeof p);
}

// MGA-INI-POS_LLH: degrees x 1e7, altitude unknown (0), accuracy in cm.
inline Frame positionLlh(double lat, double lon, uint32_t accuracy_cm = kPositionAccuracyCm) {
  if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180 ||
      (lat == 0 && lon == 0))
    return Frame();
  uint8_t p[20] = {0x01, 0x00};
  put32(p + 4, (uint32_t)(int32_t)lround(lat * 1e7));
  put32(p + 8, (uint32_t)(int32_t)lround(lon * 1e7));
  put32(p + 16, accuracy_cm);
  return frame(0x13, 0x40, p, sizeof p);
}

}  // namespace ubxassist
