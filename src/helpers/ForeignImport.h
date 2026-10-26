// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Imports from other MeshCore firmware: their files on the SD card turned into
// the MeshCore app's backup JSON (meshcore-backup.json), which MyMesh::
// uiImportBackup then applies like any backup (identity, channels, contacts).
// Platform-neutral and host-tested (test/test_foreign_import.cpp).
//
//   Squatch Mesh (BamBam1121/inw-mesh, T-Lora Pager): its SD mirror /inw keeps
//     MeshCore's own companion files, the same as Wadamesh's /meshcomod:
//       identity/_main.id  public key (32), private key (64), [name (32)]
//       contacts3          152-byte records (DataStore::loadContacts)
//       channels2          68-byte records: 4 unused, name (32), secret (32)
//   MeshPunk (PhilMo6/meshpunk, T-Deck): /meshpunk on the SD card:
//       identity           public key (32), private key (64)
//       contacts.bin       143-byte records: public key, name (32), type,
//                          flags, path length, path (64), last advert, lat, lon
//       channels           text: "slot<TAB>name<TAB>secret hex" per line, slots
//                          1 and up; Public is implied unless a "pubdel" line
//
// The output is written through `emit` a piece at a time, so a big contact
// list goes straight to a file without being held in memory.

#include <stddef.h>
#include <stdint.h>

namespace foreign {

typedef bool (*Emit)(void* ctx, const char* s, size_t n);

struct Counts {
  int contacts;
  int channels;
  bool identity;
  Counts() : contacts(0), channels(0), identity(false) {}
};

constexpr size_t kCompanionContactRec = 152;
constexpr size_t kCompanionChannelRec = 68;
constexpr size_t kMeshPunkContactRec = 143;
constexpr size_t kIdentityBytes = 96;   // public + private key

// MeshCore's Public channel key (hex).
constexpr const char* kPublicSecretHex = "8b3387e9c5cdea6ac9e5edbaa115cd72";

// MeshCore companion files (Squatch Mesh's /inw). Any of the three may be
// missing (null / 0). False only if `emit` failed.
bool companionToBackup(const uint8_t* identity, size_t id_len, const uint8_t* contacts3, size_t contacts_len,
                       const uint8_t* channels2, size_t channels_len, Emit emit, void* ctx, Counts* counts);

// MeshPunk's files. Any may be missing.
bool meshPunkToBackup(const uint8_t* identity, size_t id_len, const uint8_t* contacts, size_t contacts_len,
                      const char* channels, size_t channels_len, Emit emit, void* ctx, Counts* counts);

// MeshCore Open's contacts export (a JSON list of {"publicKey": base64, ...}):
// a contact's 32-byte key from base64. False if it isn't one.
bool base64Key32(const char* b64, uint8_t out[32]);

}  // namespace foreign
