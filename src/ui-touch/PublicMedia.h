// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Photos, voice messages and push-to-talk on Public and #public.
//
// Everyone on the mesh shares those two channels, and photos and voice there
// are clutter for everyone (other apps show them as IMAGE / VOICE lines). So
// they are off there unless the user switches them on in Settings > MediaMesh,
// which warns in red to do that only in an emergency. OTP refuses Public the
// same way (OtpCore::isPublicChannel), and this uses the same name rule.
// MeshCore Open TAK has the same switch (Settings > Messaging), also off.

#include "VoxWire.h"

namespace PublicMedia {

// "public" or "#public", in any case: the channels everyone shares. The same
// rule as OTP's, so the two refuse the same channels.
inline bool isPublicName(const char* name) {
  if (!name) return false;
  while (*name == '#') name++;
  const char* p = "public";
  for (; *name && *p; name++, p++) {
    char c = *name;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != *p) return false;
  }
  return !*name && !*p;
}

// A photo (VI1) or voice message (VE3, VM1): what this refuses. Text, team
// data and everything else on Public is untouched.
inline bool isMediaText(const char* text) {
  VoxWire::Envelope e;
  return VoxWire::parseEnvelope(text, &e);
}

// Whether photos and voice are refused on the channel named `name`, given
// the switch (`allowed`: on in an emergency).
inline bool blocks(const char* name, bool allowed) {
  return !allowed && isPublicName(name);
}

// Shown when a photo, voice message or talk is refused.
constexpr const char* kBlockedAlert =
    "Photos and voice are off on Public. Use another channel, or in an emergency turn them on in "
    "Settings > MediaMesh.";

}  // namespace PublicMedia
