// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxMic.h"

#include "VoxVoice.h"
#include "../helpers/PsramStatic.h"   // P4_PSRAM_BSS

#if defined(HAS_TDECK_GT911) && VOX_CODEC2_AVAILABLE
#define VOX_MIC_TDECK 1
#else
#define VOX_MIC_TDECK 0
#endif
// T-Display P4: the ES8311's ADC, through P4Audio (variants/tdisplay_p4).
#if defined(HAS_TDISPLAY_P4) && VOX_CODEC2_AVAILABLE
#define VOX_MIC_P4 1
#else
#define VOX_MIC_P4 0
#endif

#if VOX_MIC_TDECK || VOX_MIC_P4

#include <Arduino.h>
#if VOX_MIC_TDECK
#include <Wire.h>
#include <driver/i2s.h>
#else
#include "P4Audio.h"
#endif
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace {

using VoxMic::State;
using VoxMic::kIdle;
using VoxMic::kRecording;
using VoxMic::kReady;
using VoxMic::kFailed;

#if VOX_MIC_TDECK
// ---- ES7210 ------------------------------------------------------------------
// Register sequence from Espressif's ES7210 driver (ESP-ADF, MIT licence,
// Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.), as LilyGo ships
// it for the T-Deck (lib/es7210 in Xinyuan-LilyGO/T-Deck): slave mode, I2S
// format, 16-bit, 16 kHz from a 4.096 MHz MCLK (256 fs), all four ADCs powered
// with ADC1/2 on SDOUT1, the one data line the T-Deck wires (GPIO14).

constexpr uint8_t kEs7210Addr = 0x40;
constexpr int kPinMclk = 48, kPinBck = 47, kPinWs = 21, kPinDin = 14;
constexpr i2s_port_t kMicPort = I2S_NUM_1;   // I2S_NUM_0 is the speaker

// PGA gain for the mic inputs, in the ES7210's 3 dB steps (9 = 27 dB). The
// digital AGC in VoxMicPrep adds up to another 27 dB on top, so this only has
// to lift a normal voice clear of the ADC's noise floor without clipping a
// shout. Hardware-tune here if clips come out noisy (raise) or distorted (lower).
constexpr uint8_t kMicGain = 9;

bool wr(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

int rd(uint8_t reg) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)kEs7210Addr, 1) != 1) return -1;
  return Wire.read();
}

bool upd(uint8_t reg, uint8_t mask, uint8_t v) {
  const int cur = rd(reg);
  if (cur < 0) return false;
  return wr(reg, (uint8_t)((cur & ~mask) | (mask & v)));
}

// All four inputs on with their PGAs (es7210_mic_select with MIC1..MIC4).
bool micSelect() {
  bool ok = true;
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x10, 0x00);
  ok &= wr(0x4B, 0xFF);
  ok &= wr(0x4C, 0xFF);
  ok &= upd(0x01, 0x0B, 0x00);
  ok &= wr(0x4B, 0x00);
  ok &= upd(0x01, 0x15, 0x00);
  ok &= wr(0x4C, 0x00);
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x10, 0x10);
  return ok;
}

bool es7210Start() {
  bool ok = true;
  ok &= wr(0x00, 0xFF);   // reset
  ok &= wr(0x00, 0x41);
  ok &= wr(0x01, 0x1F);   // clocks off while configuring
  ok &= wr(0x09, 0x30);   // chip state cycle
  ok &= wr(0x0A, 0x30);   // power-on state cycle
  ok &= wr(0x40, 0xC3);   // analog: VDDA 3.3 V, VMID 5k start
  ok &= wr(0x41, 0x70);   // MIC1/2 bias 2.87 V
  ok &= wr(0x42, 0x70);   // MIC3/4 bias 2.87 V
  // 16 kHz from MCLK 4.096 MHz: adc_div 1, doubler on, DLL on, OSR 0x20, LRCK div 256.
  ok &= wr(0x02, 0xC1);
  ok &= wr(0x07, 0x20);
  ok &= wr(0x04, 0x01);
  ok &= wr(0x05, 0x00);
  // Format: 16-bit, I2S; ADC1/2 -> SDOUT1, ADC3/4 -> SDOUT2 (not TDM).
  const int sdp = rd(0x11);
  ok &= sdp >= 0 && wr(0x11, (uint8_t)(((sdp & 0x1F) | 0x60) & 0xFC));
  ok &= wr(0x12, 0x00);
  ok &= micSelect();
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x0F, kMicGain);
  // Start: clocks on, power up, mics on, then select again (as es7210_start does).
  const int clk = rd(0x01);
  ok &= clk >= 0 && wr(0x01, (uint8_t)clk);
  ok &= wr(0x06, 0x00);
  for (int i = 0; i < 4; i++) ok &= wr((uint8_t)(0x47 + i), 0x00);
  ok &= micSelect();
  return ok;
}

void es7210Stop() {
  for (int i = 0; i < 4; i++) wr((uint8_t)(0x47 + i), 0xFF);
  wr(0x4B, 0xFF);
  wr(0x4C, 0xFF);
  wr(0x01, 0x7F);
  wr(0x06, 0x07);
}

bool i2sStart() {
  // LilyGo's tested T-Deck mic configuration (examples/Microphone), but with
  // both slots kept (MIC1 left, MIC2 right) rather than left copied to both.
  // captureTask measures what actually arrives and copes with mono too.
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = VoxMicPrep::kInputRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  cfg.chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);
  if (i2s_driver_install(kMicPort, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = kPinMclk;
  pins.bck_io_num = kPinBck;
  pins.ws_io_num = kPinWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = kPinDin;
  if (i2s_set_pin(kMicPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  i2s_zero_dma_buffer(kMicPort);
  return true;
}

// ---- the board's mic, as the capture task sees it ------------------------------
bool hwStart(const char** err) {
  if (!es7210Start()) {
    es7210Stop();
    *err = "Microphone not found";
    return false;
  }
  if (!i2sStart()) {
    es7210Stop();
    *err = "Microphone busy";
    return false;
  }
  return true;
}
// Up to `bytes` of 16 kHz PCM (stereo or mono: the capture task measures which).
bool hwRead(int16_t* buf, size_t bytes, size_t* got, uint32_t timeout_ms) {
  return i2s_read(kMicPort, buf, bytes, got, pdMS_TO_TICKS(timeout_ms)) == ESP_OK && *got > 0;
}
void hwStop() {
  i2s_driver_uninstall(kMicPort);
  es7210Stop();
}
#else   // VOX_MIC_P4
bool hwStart(const char** err) {
  if (!p4AudioCaptureBegin()) {
    *err = "Microphone not available";
    return false;
  }
  return true;
}
// 16 kHz mono (P4Audio picks the ES8311's live slot).
bool hwRead(int16_t* buf, size_t bytes, size_t* got, uint32_t timeout_ms) {
  size_t n = 0;
  const bool ok = p4AudioCaptureRead(buf, bytes / sizeof(int16_t), &n, timeout_ms);
  *got = n * sizeof(int16_t);
  return ok;
}
void hwStop() { p4AudioCaptureEnd(); }
#endif

// ---- capture task --------------------------------------------------------------

constexpr size_t kMaxBits = (size_t)kVoxMaxRecordSeconds * 3200 / 8;   // 20 s at up to 3200 bps
constexpr uint32_t kSettleMs = 150;   // the ADC's power-up thump (also used to measure the stream)

volatile State s_state = kIdle;
volatile bool s_stop = false;
volatile bool s_cancel = false;
volatile bool s_task_running = false;
volatile uint32_t s_frames = 0;       // codec frames captured
volatile int s_bpf = 6;               // bytes in one of them
int s_mode = kVoxRecordWireMode;      // VE3 wire mode of the recording
uint32_t s_frame_ms = 40;             // 40 ms frames, 20 ms at 2400/3200
volatile int s_level = 0;
const char* s_error = "";
uint8_t* s_bits = nullptr;
size_t s_len = 0;
bool s_codec_on = false;

// The capture task's stack, from internal RAM (FreeRTOS here won't take a
// PSRAM stack). lib/codec2 keeps its big scratch arrays in the codec instance
// (PSRAM) rather than on the stack, so the encoder needs ~8 KB, not ~19 KB
// (test/test_codec2_stack.c). It used to be 28 KB, more than one contiguous
// block a busy T-Deck could spare.
constexpr uint32_t kTaskStack = 16 * 1024;   // ~10 KB used; the rest is margin (the high-water mark is logged)
// Plus the I2S driver: 8 DMA buffers of 128 stereo frames (4 KB) and its state.
constexpr size_t kDriverBytes = 8 * 1024;
// What has to stay free for everything else while recording (Wi-Fi, BLE, LVGL).
constexpr size_t kHeadroom = 8 * 1024;
char s_mem_err[80];

// "Not enough memory to record (41K free, 12K block, low)": what was short and
// where it failed, shown on screen so it can be reported without a serial log.
void memError(const char* where) {
  snprintf(s_mem_err, sizeof s_mem_err, "Not enough memory to record (%uK free, %uK block, %s)",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           where);
  Serial.printf("[VOX] %s\n", s_mem_err);
}

void captureJob(void*) {
  VoxMicPrep prep;
  VoxCodec2Encoder enc;
  bool ok = enc.begin(s_mode);
  if (!ok) s_error = "Codec2 couldn't start";
  const int spf = ok ? enc.samplesPerFrame() : 320;
  const int bpf = ok ? enc.bytesPerFrame() : 6;
  s_bpf = bpf;
  const size_t max_frames = ok ? (size_t)kVoxMaxRecordSeconds * 8000 / (size_t)spf : 0;
  s_frame_ms = (uint32_t)spf / 8;
  P4_PSRAM_BSS static int16_t rx[2 * 256];         // 256 stereo frames = 16 ms
  P4_PSRAM_BSS static int16_t pcm8[2 * 256 / 2 + 2];   // 8 kHz out of one read, stereo or mono
  P4_PSRAM_BSS static int16_t frame[320];
  int have = 0;
  // The first kSettleMs are thrown away (the ADC's power-up thump). They also
  // tell us what the driver delivers: 16 kHz stereo is 64000 B/s, mono 32000.
  bool settling = true, mono = false;
  size_t settle_bytes = 0;
  int64_t settle_t0 = 0;
  // Drain what the DMA buffered before this task ran, so the measurement only
  // sees samples arriving in real time.
  for (int i = 0; ok && i < 64; i++) {
    size_t got = 0;
    if (!hwRead(rx, sizeof rx, &got, 0)) break;
  }
  while (ok && !s_stop && !s_cancel) {
    size_t got = 0;
    if (!hwRead(rx, sizeof rx, &got, 200)) {
      s_error = "The microphone stopped";
      ok = false;
      break;
    }
    if (settling) {
      const int64_t t = esp_timer_get_time();
      if (!settle_t0) { settle_t0 = t; continue; }   // the first read may be stale DMA
      settle_bytes += got;
      const int64_t us = t - settle_t0;
      if (us < (int64_t)kSettleMs * 1000) continue;
      const int64_t bps = (int64_t)settle_bytes * 1000000 / us;
      mono = bps < 48000;
      settling = false;
      Serial.printf("[VOX] mic stream %lld B/s: %s\n", (long long)bps, mono ? "mono" : "stereo");
      continue;
    }
    const size_t n = mono ? prep.processMono(rx, got / sizeof(int16_t), pcm8, sizeof pcm8 / sizeof pcm8[0])
                          : prep.process(rx, got / (2 * sizeof(int16_t)), pcm8, sizeof pcm8 / sizeof pcm8[0]);
    for (size_t i = 0; i < n; i++) {
      frame[have++] = pcm8[i];
      if (have == spf) {
        have = 0;
        if (s_frames < max_frames && (s_frames + 1) * (size_t)bpf <= kMaxBits) {
          enc.encode(frame, s_bits + (size_t)s_frames * (size_t)bpf);
          // Published after the bytes, for liveBits() on the loop task.
          __atomic_store_n(&s_frames, s_frames + 1, __ATOMIC_RELEASE);
        }
      }
    }
    const int pk = prep.takePeak();
    const int lv = pk >= 32000 ? 100 : pk * 100 / 32000;
    if (lv > s_level) s_level = lv;
    if (s_frames >= max_frames) break;   // 20 s: stop as if released
  }
  s_len = (size_t)s_frames * (size_t)bpf;
  enc.end();
  // How close the stack came to its end, for tuning kTaskStack.
  Serial.printf("[VOX] mic task stack: %u bytes unused of %u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)kTaskStack);
  s_state = s_cancel ? kIdle : ok ? kReady : kFailed;
  s_task_running = false;
}

void captureTask(void* arg) {   // the fallback, on a task of its own
  captureJob(arg);
  vTaskDelete(nullptr);
}

// ---- the audio worker ------------------------------------------------------------
// One task, created at boot while the internal heap is still whole, that runs
// recordings and voice playback (never both: the UI keeps them apart). Making
// the stack on demand needed a free 16 KB block at the moment the mic was
// pressed, which a T-Deck with Wi-Fi up and a while of uptime may not have;
// this one is held for good instead.
constexpr uint32_t kWorkerStack = 16 * 1024;
TaskHandle_t s_worker = nullptr;
void (*volatile s_job)(void*) = nullptr;
void* volatile s_job_arg = nullptr;
volatile bool s_worker_busy = false;

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    void (*fn)(void*) = s_job;
    if (fn) fn(s_job_arg);
    s_job = nullptr;
    s_worker_busy = false;
  }
}

void shutdownHardware() {
  if (s_codec_on) hwStop();
  s_codec_on = false;
}

void dropClip() {
  if (s_bits) heap_caps_free(s_bits);
  s_bits = nullptr;
  s_len = 0;
  s_frames = 0;
}

// The capture task ends on its own (stop, cancel, 20 s, error); the hardware is
// released here, on the loop task, once it has.
void reap() {
  static bool hw_up = false;
  if (s_task_running) { hw_up = true; return; }
  if (hw_up) {
    shutdownHardware();
    hw_up = false;
  }
}

// ---- raw capture, for Sound link (ggwave) -----------------------------------------
volatile bool s_raw = false;     // the mic is Sound link's
bool s_raw_settling = true;
bool s_raw_mono = false;
size_t s_raw_bytes = 0;
int64_t s_raw_t0 = 0;
int s_raw_drain = 0;

}  // namespace

namespace VoxMic {

bool available() { return true; }

bool start(int wire_mode, const char** err) {
  reap();
  if (s_raw) { *err = "Sound link is listening: turn it off first"; return false; }
  if (s_task_running || s_state == kRecording) { *err = "Already recording"; return false; }
  dropClip();
  s_error = "";
  s_mode = voxRecordModeOk(wire_mode) ? wire_mode : kVoxRecordWireMode;
  s_frame_ms = (s_mode == 2 || s_mode == 6) ? 20 : 40;
  // The capture task's stack and the I2S DMA buffers both come from internal
  // RAM; refuse cleanly rather than let the driver abort(). The stack needs one
  // contiguous block, so check the largest block as well as the total. (This
  // used to demand 72 KB free in total, more than a T-Deck with Wi-Fi, BLE
  // and the UI up usually has, so recording was refused on a healthy device.)
  // With the audio worker (made at boot) only the I2S driver needs internal RAM
  // now; without it, the stack does too, in one block.
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t big_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const bool worker = VoxAudioTask::available();
  if (worker ? (big_int < 4096 || free_int < kDriverBytes + kHeadroom)
             : (big_int < kTaskStack + 1024 || free_int < kTaskStack + kDriverBytes + kHeadroom)) {
    memError(worker ? "low" : "no worker");
    *err = s_mem_err;
    return false;
  }
  s_bits = (uint8_t*)heap_caps_malloc(kMaxBits, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_bits) s_bits = (uint8_t*)heap_caps_malloc(kMaxBits, MALLOC_CAP_8BIT);
  if (!s_bits) { memError("clip"); *err = s_mem_err; return false; }
  if (!hwStart(err)) {
    dropClip();
    return false;
  }
  s_codec_on = true;
  s_stop = s_cancel = false;
  s_level = 0;
  s_state = kRecording;
  s_task_running = true;
  if (!VoxAudioTask::run(captureJob, nullptr) &&
      xTaskCreate(captureTask, "vox_mic", kTaskStack, nullptr, 3, nullptr) != pdPASS) {
    s_task_running = false;
    s_state = kIdle;
    shutdownHardware();
    dropClip();
    memError("no task");
    *err = s_mem_err;
    return false;
  }
  reap();   // note the hardware is up
  Serial.printf("[VOX] recording, mode %d\n", s_mode);
  return true;
}

void stop() { s_stop = true; }

void cancel() {
  s_cancel = true;
  if (!s_task_running && s_state != kRecording) {
    dropClip();
    s_state = kIdle;
  }
}

State state() {
  reap();
  if (s_state == kIdle && s_bits && !s_task_running) dropClip();   // a cancelled recording
  return s_state;
}

const char* error() { return s_error; }

uint32_t elapsedMs() { return s_frames * s_frame_ms; }

bool liveBits(const uint8_t** bits, size_t* len) {
  if (!bits || !len || !s_bits || (s_state != kRecording && s_state != kReady)) return false;
  const uint32_t frames = __atomic_load_n(&s_frames, __ATOMIC_ACQUIRE);
  *bits = s_bits;
  *len = (size_t)frames * (size_t)s_bpf;
  return true;
}

int level() {
  const int v = s_level;
  s_level = v * 3 / 4;
  return v;
}

bool take(uint8_t* mode, uint8_t** bits, size_t* len, uint32_t* ms, uint32_t min_ms) {
  reap();
  if (s_state != kReady || !s_bits) return false;
  s_state = kIdle;
  const uint32_t dur = s_frames * s_frame_ms;
  Serial.printf("[VOX] recorded %lu ms, %u bytes\n", (unsigned long)dur, (unsigned)s_len);
  if (dur < min_ms || s_len == 0) {
    dropClip();
    return false;
  }
  *mode = (uint8_t)s_mode;
  *bits = s_bits;
  *len = s_len;
  *ms = dur;
  s_bits = nullptr;   // the caller frees it
  s_len = 0;
  s_frames = 0;
  return true;
}

bool rawStart(const char** err) {
  reap();
  if (s_raw) return true;
  if (s_task_running || s_state == kRecording) { *err = "The microphone is recording"; return false; }
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kDriverBytes + kHeadroom) {
    memError("low");
    *err = s_mem_err;
    return false;
  }
  if (!hwStart(err)) return false;
  s_raw_settling = true;
  s_raw_mono = false;
  s_raw_bytes = 0;
  s_raw_t0 = 0;
  s_raw_drain = 64;
  s_raw = true;
  return true;
}

bool rawRead(int16_t* mono, size_t max, size_t* got, uint32_t timeout_ms) {
  *got = 0;
  if (!s_raw) return false;
  P4_PSRAM_BSS static int16_t rx[2 * 256];
  // Drain what the DMA held before, then measure what arrives (as captureJob does).
  while (s_raw_drain > 0) {
    size_t n = 0;
    s_raw_drain--;
    if (!hwRead(rx, sizeof rx, &n, 0)) { s_raw_drain = 0; break; }
  }
  size_t want = s_raw_settling || !s_raw_mono ? max * 2 : max;
  if (want > sizeof rx / sizeof rx[0]) want = sizeof rx / sizeof rx[0];
  size_t bytes = 0;
  if (!hwRead(rx, want * sizeof(int16_t), &bytes, timeout_ms)) return false;
  if (s_raw_settling) {
    const int64_t t = esp_timer_get_time();
    if (!s_raw_t0) { s_raw_t0 = t; return true; }
    s_raw_bytes += bytes;
    if (t - s_raw_t0 < (int64_t)kSettleMs * 1000) return true;
    s_raw_mono = (int64_t)s_raw_bytes * 1000000 / (t - s_raw_t0) < 48000;
    s_raw_settling = false;
    Serial.printf("[VOX] sound link mic: %s\n", s_raw_mono ? "mono" : "stereo");
    return true;
  }
  const size_t n = bytes / sizeof(int16_t);
  if (s_raw_mono) {
    memcpy(mono, rx, n * sizeof(int16_t));
    *got = n;
  } else {
    for (size_t i = 0; i + 1 < n; i += 2) mono[i / 2] = (int16_t)(((int32_t)rx[i] + rx[i + 1]) / 2);
    *got = n / 2;
  }
  return true;
}

void rawStop() {
  if (!s_raw) return;
  hwStop();
  s_raw = false;
}

bool rawActive() { return s_raw; }

}  // namespace VoxMic

namespace VoxAudioTask {

bool begin() {
  if (s_worker) return true;
  if (xTaskCreate(workerTask, "vox_audio", kWorkerStack, nullptr, 3, &s_worker) != pdPASS) {
    s_worker = nullptr;
    Serial.println("[VOX] audio worker: no memory at boot; recording will make its own task");
    return false;
  }
  return true;
}

bool exists() { return s_worker != nullptr; }
bool available() { return s_worker && !s_worker_busy; }

bool run(void (*fn)(void*), void* arg) {
  if (!s_worker || s_worker_busy || !fn) return false;
  s_worker_busy = true;
  s_job_arg = arg;
  s_job = fn;
  xTaskNotifyGive(s_worker);
  return true;
}

}  // namespace VoxAudioTask

#else  // no microphone on this board

#if defined(ESP32) || defined(ESP_PLATFORM)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// No mic, but the same worker: its 16 KB internal stack, made at boot while the
// heap is whole, is what Wi-Fi updates, the Reader and voice playback run on.
// Without it each made its own 16 KB task when needed, and on a Pager with
// Wi-Fi and BLE up the largest free block was 10 KB ("out of memory (RAM
// 18K/10K, need 16K)").
namespace {
constexpr uint32_t kWorkerStack = 16 * 1024;
TaskHandle_t s_worker = nullptr;
void (*volatile s_job)(void*) = nullptr;
void* volatile s_job_arg = nullptr;
volatile bool s_worker_busy = false;

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    void (*fn)(void*) = s_job;
    if (fn) fn(s_job_arg);
    s_job = nullptr;
    s_worker_busy = false;
  }
}
}  // namespace

namespace VoxAudioTask {
bool begin() {
  if (s_worker) return true;
  if (xTaskCreate(workerTask, "vox_audio", kWorkerStack, nullptr, 3, &s_worker) != pdPASS) {
    s_worker = nullptr;
    Serial.println("[VOX] worker: no memory at boot");
    return false;
  }
  return true;
}
bool exists() { return s_worker != nullptr; }
bool available() { return s_worker && !s_worker_busy; }
bool run(void (*fn)(void*), void* arg) {
  if (!s_worker || s_worker_busy || !fn) return false;
  s_worker_busy = true;
  s_job_arg = arg;
  s_job = fn;
  xTaskNotifyGive(s_worker);
  return true;
}
}  // namespace VoxAudioTask
#else
namespace VoxAudioTask {
bool begin() { return false; }
bool exists() { return false; }
bool available() { return false; }
bool run(void (*)(void*), void*) { return false; }
}  // namespace VoxAudioTask
#endif

namespace VoxMic {
bool available() { return false; }
bool start(int, const char** err) { *err = "This radio has no microphone"; return false; }
void stop() {}
void cancel() {}
State state() { return kIdle; }
const char* error() { return ""; }
uint32_t elapsedMs() { return 0; }
bool liveBits(const uint8_t**, size_t*) { return false; }
int level() { return 0; }
bool take(uint8_t*, uint8_t**, size_t*, uint32_t*, uint32_t) { return false; }
bool rawStart(const char** err) { *err = "This radio has no microphone"; return false; }
bool rawRead(int16_t*, size_t, size_t* got, uint32_t) { *got = 0; return false; }
void rawStop() {}
bool rawActive() { return false; }
}  // namespace VoxMic

#endif
