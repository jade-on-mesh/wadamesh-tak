// SPDX-License-Identifier: GPL-3.0-or-later
// The audio worker (VoxMic.h): one task, made at boot while the internal heap
// is still whole, that runs recording, voice and walkie playback, Sound link,
// Wi-Fi updates, the Reader and (when internal RAM is short) photo decoding.
// Making a 16 KB stack on demand needs a free 16 KB block at that moment, which
// a T-Deck or Pager with Wi-Fi and BLE up often hasn't (largest block 1-10 KB:
// "Not enough memory to play"); this one is held for good instead.
//
// runNext() queues one job behind the running one, for playback someone just
// tapped: it runs as soon as the worker is free, ahead of anything else, and the
// running job (a photo decode runs at idle priority) is raised to the worker's
// priority so it finishes sooner.
#include "VoxMic.h"

#if defined(ESP32) || defined(ESP_PLATFORM)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr uint32_t kWorkerStack = 16 * 1024;
constexpr UBaseType_t kPriority = 3;
TaskHandle_t s_worker = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// All under s_mux.
void (*s_job)(void*) = nullptr;     // handed over by run() / runNext() when idle
void* s_job_arg = nullptr;
void (*s_next)(void*) = nullptr;    // queued by runNext() while busy
void* s_next_arg = nullptr;
bool s_busy = false;
// post(): background jobs that wait their turn, after s_job and s_next.
constexpr int kFifo = 8;
void (*s_fifo_fn[kFifo])(void*) = {};
void* s_fifo_arg[kFifo] = {};
int s_fifo_head = 0, s_fifo_n = 0;
bool s_cur_bg = false;   // the running job came from post(): it gives way (see run)

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      void (*fn)(void*) = nullptr;
      void* arg = nullptr;
      portENTER_CRITICAL(&s_mux);
      s_cur_bg = false;
      if (s_job) {
        fn = s_job; arg = s_job_arg; s_job = nullptr;
      } else if (s_next) {
        fn = s_next; arg = s_next_arg; s_next = nullptr;
      } else if (s_fifo_n) {
        fn = s_fifo_fn[s_fifo_head]; arg = s_fifo_arg[s_fifo_head];
        s_fifo_head = (s_fifo_head + 1) % kFifo;
        s_fifo_n--;
        s_cur_bg = true;
      } else {
        s_busy = false;
      }
      portEXIT_CRITICAL(&s_mux);
      if (!fn) break;
      vTaskPrioritySet(nullptr, kPriority);   // a hurried job may have left it raised
      fn(arg);
    }
  }
}
}  // namespace

namespace VoxAudioTask {
bool begin() {
  if (s_worker) return true;
  if (xTaskCreate(workerTask, "vox_audio", kWorkerStack, nullptr, kPriority, &s_worker) != pdPASS) {
    s_worker = nullptr;
    Serial.println("[VOX] audio worker: no memory at boot; jobs will make their own tasks");
    return false;
  }
  return true;
}

bool exists() { return s_worker != nullptr; }

// Idle, or running background work (post()) that will give way to a run().
bool available() {
  portENTER_CRITICAL(&s_mux);
  const bool idle = s_worker && (!s_busy || (s_cur_bg && !s_next && !s_job));
  portEXIT_CRITICAL(&s_mux);
  return idle;
}

bool run(void (*fn)(void*), void* arg) {
  if (!s_worker || !fn) return false;
  portENTER_CRITICAL(&s_mux);
  bool ok = !s_busy;
  if (ok) {
    s_busy = true;
    s_job = fn;
    s_job_arg = arg;
  } else if (s_cur_bg && !s_job && !s_next) {
    // Background work is running (map tiles, a settings save): this goes
    // next, and that job gives way at its next step (it checks waiting()).
    s_job = fn;
    s_job_arg = arg;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (ok) xTaskNotifyGive(s_worker);
  return ok;
}

bool runNext(void (*fn)(void*), void* arg) {
  if (!s_worker || !fn) return false;
  bool now = false, queued = false;
  portENTER_CRITICAL(&s_mux);
  if (!s_busy) {
    s_busy = true;
    s_job = fn;
    s_job_arg = arg;
    now = true;
  } else if (!s_next) {
    s_next = fn;
    s_next_arg = arg;
    queued = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (now) {
    xTaskNotifyGive(s_worker);
    return true;
  }
  if (!queued) return false;
  // Hurry the job in the way. (A decode at idle priority restores the worker's
  // priority when it ends; the worker sets it again before each job.)
  if (uxTaskPriorityGet(s_worker) < kPriority) vTaskPrioritySet(s_worker, kPriority);
  Serial.println("[VOX] audio worker busy: playing as soon as its job ends");
  return true;
}

bool queued() {
  portENTER_CRITICAL(&s_mux);
  const bool q = s_next != nullptr;
  portEXIT_CRITICAL(&s_mux);
  return q;
}

bool post(void (*fn)(void*), void* arg) {
  if (!s_worker || !fn) return false;
  bool ok = false, wake = false;
  portENTER_CRITICAL(&s_mux);
  if (s_fifo_n < kFifo) {
    const int at = (s_fifo_head + s_fifo_n) % kFifo;
    s_fifo_fn[at] = fn;
    s_fifo_arg[at] = arg;
    s_fifo_n++;
    ok = true;
    if (!s_busy) { s_busy = true; wake = true; }
  }
  portEXIT_CRITICAL(&s_mux);
  if (wake) xTaskNotifyGive(s_worker);
  return ok;
}

bool waiting() {
  portENTER_CRITICAL(&s_mux);
  const bool w = s_job != nullptr || s_next != nullptr || s_fifo_n > 0;
  portEXIT_CRITICAL(&s_mux);
  return w;
}

bool isWorker() { return s_worker && xTaskGetCurrentTaskHandle() == s_worker; }
}  // namespace VoxAudioTask

#else  // host builds (web)

namespace VoxAudioTask {
bool begin() { return false; }
bool exists() { return false; }
bool available() { return false; }
bool run(void (*)(void*), void*) { return false; }
bool runNext(void (*)(void*), void*) { return false; }
bool queued() { return false; }
bool post(void (*)(void*), void*) { return false; }
bool waiting() { return false; }
bool isWorker() { return false; }
}  // namespace VoxAudioTask

#endif
