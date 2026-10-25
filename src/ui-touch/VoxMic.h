// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Records MeshCore Open TAK voice clips on the radio: the board's microphone,
// through VoxMicPrep, encoded to Codec2 (1200, 1600 or 3200) as it's captured, so a clip is
// ready the moment the user lets go.
//
// LilyGo T-Deck / T-Deck Plus: ES7210 mic ADC (I2C 0x40, MIC1+MIC2 on SDOUT1 ->
// GPIO14), clocked by the S3 as I2S master on I2S_NUM_1 (MCLK 48, BCK 47,
// WS 21) at 16 kHz. I2S_NUM_0 stays the speaker's, so the two never contend.
// Other boards: available() is false and the chat keeps a plain send button.
//
// start()/stop()/cancel()/take() run on the Arduino loop task (they touch the
// I2C bus the touch panel and keyboard also use); capture and encoding run on
// their own task.

#include <stddef.h>
#include <stdint.h>

namespace VoxMic {

enum State : uint8_t {
  kIdle = 0,
  kRecording,   // capturing and encoding
  kReady,       // stopped; take() the clip
  kFailed,      // couldn't start or the capture failed; error() says why
};

// Whether this build has a microphone it can record from.
bool available();

// Starts recording in `wire_mode` (a VE3 id, see voxRecordModeOk; anything else
// records 1200). False with `err` set when it can't (busy, no memory...).
bool start(int wire_mode, const char** err);
// Stops; the clip becomes ready (state kReady) within a frame.
void stop();
// Stops and throws the recording away.
void cancel();

State state();
const char* error();
// Length of audio captured so far (or of the finished clip).
uint32_t elapsedMs();
// While recording (or stopped, before take()): the Codec2 encoded so far, for
// push-to-talk, which sends each second as soon as it's there. Bytes below
// `len` don't change any more. Loop task only.
bool liveBits(const uint8_t** bits, size_t* len);
// Input level for a meter, 0..100 (decays between reads).
int level();

// Hands over a finished clip: VE3 wire mode, codec frames (free() them), and
// its length. False (and the recording dropped) if it's shorter than `min_ms`.
bool take(uint8_t* mode, uint8_t** bits, size_t* len, uint32_t* ms, uint32_t min_ms = 600);

// Raw capture for Sound link (ggwave): 16 kHz mono, whatever the board's mic
// delivers. rawStart()/rawStop() on the loop task (the T-Deck's mic ADC is on
// the I2C bus), rawRead() on one other task. Recording refuses while it runs,
// and rawStart() while recording. rawRead() gives up to `max` samples (none
// for the first 150 ms, while the ADC settles); false when the mic stopped.
bool rawStart(const char** err);
bool rawRead(int16_t* mono, size_t max, size_t* got, uint32_t timeout_ms);
void rawStop();
bool rawActive();

}  // namespace VoxMic

// A task for recording, voice playback, Wi-Fi updates and the Reader, created
// once at boot (begin()) so its 16 KB internal-RAM stack is there when needed,
// however fragmented the heap has become since. Every ESP32 board has it.
namespace VoxAudioTask {
bool begin();
// The worker was made (it may be busy).
bool exists();
// The worker exists and is idle.
bool available();
// Runs fn(arg) on the worker (fn returns; it must not delete its task). False
// if there's no worker or it's busy (or a job is queued): make a task of your
// own then.
bool run(void (*fn)(void*), void* arg);
// For playback someone is waiting for: runs fn(arg) now if the worker is idle,
// else queues it to run next, ahead of anything else, and raises the running
// job (a photo decode runs at idle priority) so it ends sooner. False if
// there's no worker or another job is already queued.
bool runNext(void (*fn)(void*), void* arg);
// A job is waiting for the worker (queued by runNext).
bool queued();
}  // namespace VoxAudioTask
