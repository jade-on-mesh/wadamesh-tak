// SPDX-License-Identifier: GPL-3.0-or-later
// Imports from other MeshCore firmware (ForeignImport.h).

#include "ForeignImport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace foreign {
namespace {

struct Out {
  Emit emit;
  void* ctx;
  bool ok = true;
  void put(const char* s, size_t n) { if (ok && n) ok = emit(ctx, s, n); }
  void put(const char* s) { put(s, strlen(s)); }
  void hex(const uint8_t* b, size_t n) {
    static const char* d = "0123456789abcdef";
    char buf[64];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      buf[k++] = d[b[i] >> 4];
      buf[k++] = d[b[i] & 15];
      if (k == sizeof buf) { put(buf, k); k = 0; }
    }
    put(buf, k);
  }
  // A JSON string from up to `cap` bytes (stops at a NUL).
  void str(const char* s, size_t cap) {
    put("\"");
    for (size_t i = 0; i < cap && s[i]; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (c == '"' || c == '\\') { const char e[2] = {'\\', (char)c}; put(e, 2); }
      else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); put(e); }
      else put(&s[i], 1);
    }
    put("\"");
  }
  void num(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); put(b); }
  void deg(int32_t micro) {   // as the MeshCore app writes them: a string
    char b[24];
    snprintf(b, sizeof b, "\"%s%ld.%06ld\"", micro < 0 ? "-" : "", labs((long)micro) / 1000000L,
             labs((long)micro) % 1000000L);
    put(b);
  }
};

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool allZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i]) return false;
  return true;
}

// "name" and "private_key" from a public + private key file (with a name after, in a companion's).
void identity(Out& o, const uint8_t* id, size_t len, Counts* c) {
  if (!id || len < kIdentityBytes || allZero(id + 32, 64)) return;
  if (len >= kIdentityBytes + 32 && id[kIdentityBytes]) {
    o.put("\"name\":");
    o.str((const char*)id + kIdentityBytes, 32);
    o.put(",");
  }
  o.put("\"private_key\":\"");
  o.hex(id + 32, 64);
  o.put("\",\"public_key\":\"");
  o.hex(id, 32);
  o.put("\",");
  if (c) c->identity = true;
}

struct Contact {
  const uint8_t* pub;
  const char* name;
  uint8_t type, flags;
  uint32_t last_advert, lastmod;
  int32_t lat, lon;
};

void contact(Out& o, const Contact& k, bool first) {
  if (!first) o.put(",");
  o.put("{\"public_key\":\"");
  o.hex(k.pub, 32);
  o.put("\",\"name\":");
  o.str(k.name, 32);
  o.put(",\"type\":");
  o.num(k.type);
  o.put(",\"flags\":");
  o.num(k.flags);
  o.put(",\"latitude\":");
  o.deg(k.lat);
  o.put(",\"longitude\":");
  o.deg(k.lon);
  o.put(",\"last_advert\":");
  o.num((long)k.last_advert);
  o.put(",\"last_modified\":");
  o.num((long)k.lastmod);
  o.put("}");
}

void channel(Out& o, const char* name, size_t name_cap, const uint8_t* secret16, bool first) {
  if (!first) o.put(",");
  o.put("{\"name\":");
  o.str(name ? name : "", name ? name_cap : 0);
  o.put(",\"secret\":\"");
  if (secret16) o.hex(secret16, 16);
  o.put("\"}");
}

bool fromHex(const char* s, size_t n, uint8_t* out, size_t out_len) {
  if (n < out_len * 2) return false;
  for (size_t i = 0; i < out_len; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      const char c = s[2 * i + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    out[i] = (uint8_t)v;
  }
  return true;
}

}  // namespace

bool companionToBackup(const uint8_t* id, size_t id_len, const uint8_t* contacts3, size_t contacts_len,
                       const uint8_t* channels2, size_t channels_len, Emit emit, void* ctx, Counts* counts) {
  Out o{emit, ctx};
  if (counts) *counts = Counts();
  o.put("{");
  identity(o, id, id_len, counts);
  o.put("\"channels\":[");
  // Slot order kept: an empty slot is an empty entry, which the import skips.
  const size_t nch = channels2 ? channels_len / kCompanionChannelRec : 0;
  size_t last = 0;   // up to the last named slot
  for (size_t i = 0; i < nch; i++)
    if (channels2[i * kCompanionChannelRec + 4]) last = i + 1;
  for (size_t i = 0; i < last; i++) {
    const uint8_t* r = channels2 + i * kCompanionChannelRec;
    const char* nm = (const char*)r + 4;
    const bool named = nm[0] != 0;
    channel(o, named ? nm : nullptr, 32, named ? r + 36 : nullptr, i == 0);
    if (named && counts) counts->channels++;
  }
  o.put("],\"contacts\":[");
  const size_t nco = contacts3 ? contacts_len / kCompanionContactRec : 0;
  bool first = true;
  for (size_t i = 0; i < nco; i++) {
    const uint8_t* r = contacts3 + i * kCompanionContactRec;
    Contact k;
    k.pub = r;
    k.name = (const char*)r + 32;
    k.type = r[64];
    k.flags = r[65];
    // r[66] unused, r[67..70] sync_since, r[71] path length
    k.last_advert = rd32(r + 72);
    // r[76..139] path
    k.lastmod = rd32(r + 140);
    k.lat = (int32_t)rd32(r + 144);
    k.lon = (int32_t)rd32(r + 148);
    if (k.type == 0 || !k.name[0] || allZero(k.pub, 32)) continue;   // blank slot (DataStore skips them too)
    contact(o, k, first);
    first = false;
    if (counts) counts->contacts++;
  }
  o.put("],\"source\":\"companion files\"}");
  return o.ok;
}

bool meshPunkToBackup(const uint8_t* id, size_t id_len, const uint8_t* contacts, size_t contacts_len,
                      const char* channels, size_t channels_len, Emit emit, void* ctx, Counts* counts) {
  Out o{emit, ctx};
  if (counts) *counts = Counts();
  o.put("{");
  identity(o, id, id_len, counts);

  // Channels: Public in slot 0 (MeshPunk makes it at boot unless deleted), then
  // each "slot<TAB>name<TAB>secret" line in its slot.
  struct Ch { char name[32]; uint8_t secret[16]; bool used; };
  static constexpr int kSlots = 40;
  Ch* ch = (Ch*)calloc(kSlots, sizeof(Ch));
  if (!ch) return false;
  bool pub_deleted = false;
  int last = 0;
  for (size_t p = 0; channels && p < channels_len;) {
    size_t e = p;
    while (e < channels_len && channels[e] != '\n' && channels[e] != '\r') e++;
    const char* line = channels + p;
    const size_t n = e - p;
    p = e + 1;
    if (n == 6 && !strncmp(line, "pubdel", 6)) { pub_deleted = true; continue; }
    const char* t1 = (const char*)memchr(line, '\t', n);
    const char* t2 = t1 ? (const char*)memchr(t1 + 1, '\t', n - (size_t)(t1 + 1 - line)) : nullptr;
    if (!t1 || !t2) continue;
    const int slot = atoi(line);
    if (slot < 1 || slot >= kSlots) continue;
    const size_t nl = (size_t)(t2 - t1 - 1);
    Ch& c = ch[slot];
    memset(c.name, 0, sizeof c.name);
    memcpy(c.name, t1 + 1, nl < sizeof c.name - 1 ? nl : sizeof c.name - 1);
    if (!c.name[0] || !fromHex(t2 + 1, (size_t)(line + n - t2 - 1), c.secret, 16)) continue;
    c.used = true;
    if (slot + 1 > last) last = slot + 1;
  }
  if (!pub_deleted) {
    memcpy(ch[0].name, "Public", 7);
    fromHex(kPublicSecretHex, 32, ch[0].secret, 16);
    ch[0].used = true;
    if (last < 1) last = 1;
  }
  o.put("\"channels\":[");
  for (int i = 0; i < last; i++) {
    channel(o, ch[i].used ? ch[i].name : nullptr, 32, ch[i].used ? ch[i].secret : nullptr, i == 0);
    if (ch[i].used && counts) counts->channels++;
  }
  free(ch);

  o.put("],\"contacts\":[");
  const size_t nco = contacts ? contacts_len / kMeshPunkContactRec : 0;
  bool first = true;
  for (size_t i = 0; i < nco; i++) {
    const uint8_t* r = contacts + i * kMeshPunkContactRec;
    Contact k;
    k.pub = r;
    k.name = (const char*)r + 32;
    k.type = r[64];
    k.flags = r[65];
    // r[66] path length, r[67..130] path
    k.last_advert = rd32(r + 131);
    k.lat = (int32_t)rd32(r + 135);
    k.lon = (int32_t)rd32(r + 139);
    k.lastmod = 0;
    if (k.type == 0 || !k.name[0] || allZero(k.pub, 32)) continue;
    contact(o, k, first);
    first = false;
    if (counts) counts->contacts++;
  }
  o.put("],\"source\":\"MeshPunk\"}");
  return o.ok;
}

bool base64Key32(const char* s, uint8_t out[32]) {
  if (!s) return false;
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (; *s && *s != '='; s++) {
    int v;
    const char c = *s;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else return false;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= 32) return false;
      out[n++] = (uint8_t)(acc >> bits);
    }
  }
  return n == 32;
}

}  // namespace foreign
