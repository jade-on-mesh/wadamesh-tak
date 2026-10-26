// SPDX-License-Identifier: GPL-3.0-or-later
// ForeignImport: Squatch Mesh's companion files and MeshPunk's turned into the
// MeshCore app's backup JSON, checked field by field.
//   g++ -std=c++17 -I src test/test_foreign_import.cpp src/helpers/ForeignImport.cpp
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "helpers/ForeignImport.h"

static bool collect(void* ctx, const char* s, size_t n) {
  static_cast<std::string*>(ctx)->append(s, n);
  return true;
}
static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i));
}
static bool has(const std::string& s, const char* what) {
  if (s.find(what) != std::string::npos) return true;
  fprintf(stderr, "missing %s in\n%s\n", what, s.c_str());
  return false;
}

int main() {
  // ---- identity: public, private, name (a companion's _main.id)
  std::vector<uint8_t> id(96 + 32, 0);
  for (int i = 0; i < 32; i++) id[i] = (uint8_t)(0xA0 + i % 16);
  for (int i = 0; i < 64; i++) id[32 + i] = (uint8_t)i;
  memcpy(&id[96], "Tiamat", 6);

  // ---- Squatch Mesh /inw: contacts3 (two real, one blank) and channels2 (Public, a gap, #hike)
  std::vector<uint8_t> c3(152 * 3, 0);
  for (int i = 0; i < 32; i++) c3[i] = 0x11;
  memcpy(&c3[32], "Bob \"B\"", 7);
  c3[64] = 1;  c3[65] = 1;
  put32(c3, 72, 1761500000);
  put32(c3, 140, 1761500100);
  put32(c3, 144, (uint32_t)47606200);
  put32(c3, 148, (uint32_t)-122332100);
  for (int i = 0; i < 32; i++) c3[152 + i] = 0x22;
  memcpy(&c3[152 + 32], "Hilltop RPT", 11);
  c3[152 + 64] = 2;
  // record 3 blank
  std::vector<uint8_t> ch2(68 * 3, 0);
  memcpy(&ch2[4], "Public", 6);
  const uint8_t pub_psk[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
  memcpy(&ch2[36], pub_psk, 16);
  memcpy(&ch2[68 * 2 + 4], "#hike", 5);
  for (int i = 0; i < 16; i++) ch2[68 * 2 + 36 + i] = (uint8_t)(0xF0 + i);

  std::string j;
  foreign::Counts n;
  assert(foreign::companionToBackup(id.data(), id.size(), c3.data(), c3.size(), ch2.data(), ch2.size(), collect, &j, &n));
  assert(n.contacts == 2 && n.channels == 2 && n.identity);
  assert(has(j, "\"name\":\"Tiamat\""));
  assert(has(j, "\"private_key\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\""));
  assert(has(j, "\"channels\":[{\"name\":\"Public\",\"secret\":\"8b3387e9c5cdea6ac9e5edbaa115cd72\"},"
                "{\"name\":\"\",\"secret\":\"\"},{\"name\":\"#hike\",\"secret\":\"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff\"}]"));
  assert(has(j, "{\"public_key\":\"1111111111111111111111111111111111111111111111111111111111111111\","
                "\"name\":\"Bob \\\"B\\\"\",\"type\":1,\"flags\":1,\"latitude\":\"47.606200\","
                "\"longitude\":\"-122.332100\",\"last_advert\":1761500000,\"last_modified\":1761500100}"));
  assert(has(j, "\"name\":\"Hilltop RPT\",\"type\":2"));
  assert(j.front() == '{' && j.back() == '}');

  // Missing files: still a well-formed document.
  j.clear();
  assert(foreign::companionToBackup(nullptr, 0, nullptr, 0, nullptr, 0, collect, &j, &n));
  assert(j == "{\"channels\":[],\"contacts\":[],\"source\":\"companion files\"}" && n.contacts == 0 && !n.identity);

  // ---- MeshPunk /meshpunk: identity (96), contacts.bin (143), channels text
  std::vector<uint8_t> mp(143 * 2, 0);
  for (int i = 0; i < 32; i++) mp[i] = 0x33;
  memcpy(&mp[32], "Carol", 5);
  mp[64] = 1;
  mp[66] = 2;                       // path length
  put32(mp, 131, 1761400000);
  put32(mp, 135, (uint32_t)45500000);
  put32(mp, 139, (uint32_t)-122600000);
  // record 2 blank
  const char* chans = "3\t#ops\t000102030405060708090a0b0c0d0e0f0000000000000000000000000000000000\n"
                      "1\tFamily\tAABBCCDDEEFF00112233445566778899\n"
                      "bad line\n"
                      "99\tTooFar\t00112233445566778899aabbccddeeff\n";
  j.clear();
  assert(foreign::meshPunkToBackup(id.data(), 96, mp.data(), mp.size(), chans, strlen(chans), collect, &j, &n));
  assert(n.contacts == 1 && n.channels == 3 && n.identity);
  assert(j.find("\"name\":\"Tiamat\"") == std::string::npos);   // MeshPunk's identity file has no name
  assert(has(j, "\"channels\":[{\"name\":\"Public\",\"secret\":\"8b3387e9c5cdea6ac9e5edbaa115cd72\"},"
                "{\"name\":\"Family\",\"secret\":\"aabbccddeeff00112233445566778899\"},"
                "{\"name\":\"\",\"secret\":\"\"},"
                "{\"name\":\"#ops\",\"secret\":\"000102030405060708090a0b0c0d0e0f\"}]"));
  assert(has(j, "\"name\":\"Carol\",\"type\":1,\"flags\":0,\"latitude\":\"45.500000\",\"longitude\":\"-122.600000\","
                "\"last_advert\":1761400000,\"last_modified\":0"));
  // Public deleted there: not brought back.
  j.clear();
  const char* nopub = "pubdel\n2\tTeam\t00112233445566778899aabbccddeeff\n";
  assert(foreign::meshPunkToBackup(nullptr, 0, nullptr, 0, nopub, strlen(nopub), collect, &j, &n));
  assert(has(j, "\"channels\":[{\"name\":\"\",\"secret\":\"\"},{\"name\":\"\",\"secret\":\"\"},{\"name\":\"Team\""));
  assert(n.channels == 1);

  // ---- MeshCore Open's base64 keys
  uint8_t k[32];
  assert(foreign::base64Key32("ERERERERERERERERERERERERERERERERERERERERERE=", k) && k[0] == 0x11 && k[31] == 0x11);
  assert(!foreign::base64Key32("ERER", k));
  assert(!foreign::base64Key32("not base64!", k));

  // A failing sink stops it.
  auto fail = [](void*, const char*, size_t) { return false; };
  assert(!foreign::companionToBackup(id.data(), id.size(), c3.data(), c3.size(), nullptr, 0, fail, nullptr, &n));
  puts("foreign_import: all passed");
  return 0;
}
