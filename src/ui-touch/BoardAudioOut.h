// SPDX-License-Identifier: GPL-3.0-or-later
// A board's plain audio output, for boards without a codec driver of their own in
// UITask: the voice player and Lua audio write 16-bit mono PCM here
// (luaAudioSinkBegin/Write/End). HAS_BOARD_AUDIO_OUT (device_caps.h) says a board
// has one:
//   - the CrowPanel Advanced 10.1" P4: an I2S class-D amp (variants/crowpanel_p4_101/CpAudio.cpp);
//   - the ThinkNode M9 and the Heltec V4 builds: the buzzer, as 1-bit PDM on its pin
//     (BuzzerVoice.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

// The rate boardAudioStreamWrite takes.
uint32_t boardAudioStreamRate();
// Takes the output (false when it can't start). One player at a time.
bool boardAudioStreamBegin();
bool boardAudioStreamWrite(const int16_t* samples, size_t frames);
// Ends the playback and hands the output back (the buzzer to its beeps).
void boardAudioStreamEnd();
