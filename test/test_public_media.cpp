// SPDX-License-Identifier: GPL-3.0-or-later
//
// Photos and voice on Public and #public: off unless switched on.

#include <assert.h>
#include <stdio.h>

#include "ui-touch/PublicMedia.h"

int main() {
  // The channels everyone shares, by OTP's rule: Public and #public, any case.
  assert(PublicMedia::isPublicName("Public"));
  assert(PublicMedia::isPublicName("public"));
  assert(PublicMedia::isPublicName("#public"));
  assert(PublicMedia::isPublicName("#PUBLIC"));
  assert(!PublicMedia::isPublicName("#publicsafety"));
  assert(!PublicMedia::isPublicName("Team"));
  assert(!PublicMedia::isPublicName("#pub"));
  assert(!PublicMedia::isPublicName(""));
  assert(!PublicMedia::isPublicName(nullptr));
  // OtpCore::isPublicChannel's rule, case for case: leading #s, whole word.
  assert(PublicMedia::isPublicName("##Public"));
  assert(!PublicMedia::isPublicName("publi"));
  assert(!PublicMedia::isPublicName("Pub lic"));

  // Off by default: refused on Public and #public only; the switch allows them.
  assert(PublicMedia::blocks("Public", false));
  assert(PublicMedia::blocks("#public", false));
  assert(!PublicMedia::blocks("#hiking", false));
  assert(!PublicMedia::blocks("Public", true));
  assert(!PublicMedia::blocks("#public", true));

  // What is refused: photos and voice messages, captions too. Not text.
  assert(PublicMedia::isMediaText("VI1:79e6a5:14:10:46"));
  assert(PublicMedia::isMediaText("VI1:79e6a5:14:10:46 IMAGE Look at this"));
  assert(PublicMedia::isMediaText("VE3:1a2b3c:1:5:42"));
  assert(PublicMedia::isMediaText("  VE3:1a2b3c:1:5:42 hello"));
  assert(!PublicMedia::isMediaText("hello Public"));
  assert(!PublicMedia::isMediaText("VR3:abc"));        // a fetch request, not media
  assert(!PublicMedia::isMediaText("I sent VI1:79e6a5:14:10:46"));
  assert(!PublicMedia::isMediaText(nullptr));

  puts("test_public_media: ok");
  return 0;
}
