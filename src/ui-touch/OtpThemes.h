// SPDX-License-Identifier: GPL-3.0-or-later
// The six themes of OTP Messenger (OTP_3_RC1.lua), same names and colours
// (Cascadia kept to its blues, greens and white). OTP Messenger takes one as
// its accent; Settings > Theme offers all but Default as device-wide looks.
// Trans and Cascadia have white as `keyphrase` (it was a second light pink and
// a third light blue): a sender colour in OTP Messenger, and the radio-tools
// colour of the device look, so white shows in both, as in their flags.
#pragma once
#include <stdint.h>

struct OtpThemeColors {
  const char* name;
  uint32_t encrypt, decrypt, ciphertext, plaintext, newpad, channel, ok, keyphrase, padinfo, ready;
};

static const OtpThemeColors kOtpThemes[6] = {
  {"Default",   0xFF6B4A, 0x2DD4BF, 0xF97316, 0x38BDF8, 0xA78BFA, 0xEC4899, 0x4ADE80, 0xFBBF24, 0x22D3EE, 0x94A3B8},
  {"Cyberpunk", 0xFF1493, 0x39FF14, 0xBC13FE, 0x00F5FF, 0xFF00FF, 0xF5FF00, 0x39FF14, 0xFFAA00, 0x00FFEF, 0x6B7280},
  {"Trans",     0xF5A9B8, 0x5BCEFA, 0xF5A9B8, 0xFFFFFF, 0xA6E8FF, 0xFF69B4, 0x5BCEFA, 0xFFFFFF, 0x38BDF8, 0xE5E7EB},   // pinks, blues, white
  {"Cascadia",  0x60A5FA, 0x38BDF8, 0x2DD4BF, 0xFFFFFF, 0x4ADE80, 0x15803D, 0x84CC16, 0xFFFFFF, 0x22C55E, 0xDBEAFE},   // blues, greens, white
  {"B&W",       0xFFFFFF, 0xE6E6E6, 0xCCCCCC, 0xFFFFFF, 0xB3B3B3, 0xD9D9D9, 0xFFFFFF, 0x999999, 0x808080, 0x4D4D4D},
  {"Acid",      0xFF4500, 0xFFA500, 0x2F4F4F, 0xFFFFFF, 0x4682B4, 0xFF0000, 0xFFD700, 0xFFDAB9, 0x8B0000, 0x696969},
};
