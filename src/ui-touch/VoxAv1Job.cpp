// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxAv1Job.h"

#ifdef ESP_PLATFORM

#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "VoxMic.h"   // VoxAudioTask: the shared 16 KB worker
#include "VoxWire.h"

namespace VoxAv1Job {
namespace {

constexpr uint32_t kStackBytes = 24 * 1024;
// The shared worker's stack (VoxMic.cpp). The host measures ~11 KB of decode
// stack at worst; the worker is used when internal RAM has no 24 KB block.
constexpr uint32_t kWorkerStackBytes = 16 * 1024;
// Our own task only while this much is free in one block, so a decode never
// takes the last of what Wi-Fi and BLE need.
constexpr uint32_t kOwnTaskFreeBlock = kStackBytes + 8 * 1024;

std::atomic<uint8_t> g_state{kIdle};
uint8_t* g_payload = nullptr;
size_t g_len = 0;
int g_max_side = 1024;
VoxAv1Result g_result = kVoxAv1DecodeFailed;
VoxAv1Image g_img;
Stats g_stats;

uint32_t g_stack_bytes = kStackBytes;

void decodeBody() {
  const uint32_t mhz_before = getCpuFrequencyMhz();
  const bool boosted = mhz_before < 240 && setCpuFrequencyMhz(240);

  const int64_t t0 = esp_timer_get_time();
  voxAv1HeapResetPeak();
  const size_t heap_before = voxAv1HeapInUse();

  size_t obu_len = 0;
  const uint8_t* obus = VoxWire::imageObus(g_payload, g_len, &obu_len);
  g_result = obus ? voxAv1Decode(obus, obu_len, kVoxAv1Rgb565, g_max_side, &g_img) : kVoxAv1BadInput;

  g_stats.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  g_stats.heap_peak = (uint32_t)(voxAv1HeapPeak() - heap_before);
  g_stats.cpu_mhz = getCpuFrequencyMhz();
  // ESP-IDF reports the high-water mark in bytes.
  // On the worker this is the most its stack has ever held, decode or audio.
  g_stats.stack_used = g_stack_bytes - (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

  // Only undo our own boost: if something else changed the clock meanwhile, leave it.
  if (boosted && getCpuFrequencyMhz() == 240) setCpuFrequencyMhz(mhz_before);

  voxAv1Free(g_payload);
  g_payload = nullptr;
  g_state.store(kDone);
}

void decodeTask(void*) {
  decodeBody();
  vTaskDelete(nullptr);
}

// On the shared worker: at idle priority like our own task (the worker runs
// audio at 3, which would starve the UI for the whole decode).
void decodeOnWorker(void*) {
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
  decodeBody();
  vTaskPrioritySet(nullptr, prio);
}

bool startOwnTask() {
  g_stack_bytes = kStackBytes;
  return xTaskCreatePinnedToCore(decodeTask, "vox_av1", kStackBytes, nullptr, tskIDLE_PRIORITY, nullptr, 0) ==
         pdPASS;
}

bool startOnWorker() {
  g_stack_bytes = kWorkerStackBytes;
  return VoxAudioTask::available() && VoxAudioTask::run(decodeOnWorker, nullptr);
}

}  // namespace

bool start(const uint8_t* payload, size_t len, int max_side) {
  if (!payload || len < 2) return false;
  uint8_t expected = kIdle;
  if (!g_state.compare_exchange_strong(expected, kRunning)) return false;

  g_payload = (uint8_t*)voxAv1Alloc(len);
  if (!g_payload) { g_state.store(kIdle); return false; }
  memcpy(g_payload, payload, len);
  g_len = len;
  g_max_side = max_side;
  g_img = VoxAv1Image{};
  g_stats = Stats{};
  g_result = kVoxAv1DecodeFailed;
  g_stats.internal_largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_stats.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  // Our own 24 KB task while internal RAM has room for it to spare; otherwise
  // the 16 KB worker made at boot (with Wi-Fi and BLE up, a Pager's largest
  // free block can be 1 KB: "Not enough internal RAM to start the decoder").
  // The worker may be playing a clip: then try our own task anyway.
  bool started = g_stats.internal_largest >= kOwnTaskFreeBlock && startOwnTask();
  if (!started) started = startOnWorker();
  if (!started) started = startOwnTask();
  g_stats.stack_size = g_stack_bytes;
  if (!started) {
    // Not enough internal RAM for the decode stack right now, and the worker is busy.
    voxAv1Free(g_payload);
    g_payload = nullptr;
    g_result = kVoxAv1NoStack;
    g_state.store(kDone);
  }
  return true;
}

State state() { return (State)g_state.load(); }

VoxAv1Result finish(VoxAv1Image* img, Stats* stats) {
  if (g_state.load() != kDone) return kVoxAv1DecodeFailed;
  if (img) *img = g_img;
  else voxAv1Free(g_img.pixels);
  if (stats) *stats = g_stats;
  const VoxAv1Result r = g_result;
  g_img = VoxAv1Image{};
  g_state.store(kIdle);
  return r;
}

}  // namespace VoxAv1Job

#endif  // ESP_PLATFORM
