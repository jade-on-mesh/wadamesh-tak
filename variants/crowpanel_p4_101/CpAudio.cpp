// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_CROWPANEL_P4_101)
// CrowPanel Advanced 10.1" P4 speaker (BoardAudioOut.h): an I2S class-D amplifier
// on LRCK 21 / BCLK 22 / DOUT 23, its shutdown on GPIO30 (LOW = on, as Elecrow's
// lesson code; target.cpp holds it HIGH at boot). No codec to set up: the amp takes
// I2S straight in. The channel exists only while something plays, and the amp is
// on only then, so there's no idle hiss and no DMA held otherwise. Both slots carry
// the same sample, whichever one the amp is strapped to.
// From the schematics and Elecrow's lessons; not yet run on hardware (target.h).
#include "../../src/ui-touch/BoardAudioOut.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>

#define CPA_RATE 16000
#define CPA_WS   21
#define CPA_BCLK 22
#define CPA_DOUT 23
#define CPA_SD   30   // amp shutdown: LOW = on

static SemaphoreHandle_t s_mtx = nullptr;   // one player at a time
static i2s_chan_handle_t s_tx = nullptr;

uint32_t boardAudioStreamRate() { return CPA_RATE; }

bool boardAudioStreamBegin() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  if (!s_mtx || xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ch.auto_clear = true;   // an underrun sends silence, not the last buffer again
  if (i2s_new_channel(&ch, &s_tx, nullptr) != ESP_OK) {
    s_tx = nullptr;
    xSemaphoreGive(s_mtx);
    return false;
  }
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CPA_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = GPIO_NUM_NC,
      .bclk = (gpio_num_t)CPA_BCLK,
      .ws   = (gpio_num_t)CPA_WS,
      .dout = (gpio_num_t)CPA_DOUT,
      .din  = GPIO_NUM_NC,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
    printf("[CPAUDIO] I2S init failed\n");
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    xSemaphoreGive(s_mtx);
    return false;
  }
  gpio_set_level((gpio_num_t)CPA_SD, 0);   // amp on, now that the clocks run
  return true;
}

bool boardAudioStreamWrite(const int16_t* samples, size_t frames) {
  if (!samples || !frames || !s_tx) return false;
  static int16_t stereo[256 * 2];
  while (frames) {
    const size_t count = frames > 256 ? 256 : frames;
    for (size_t i = 0; i < count; ++i) stereo[2 * i] = stereo[2 * i + 1] = samples[i];
    size_t written = 0;
    const size_t bytes = count * 2 * sizeof(int16_t);
    if (i2s_channel_write(s_tx, stereo, bytes, &written, pdMS_TO_TICKS(300)) != ESP_OK ||
        written != bytes) return false;
    samples += count;
    frames -= count;
  }
  return true;
}

void boardAudioStreamEnd() {
  if (!s_mtx || !s_tx) return;
  // A little silence first, so the amp doesn't switch off mid-sample (a click).
  static const int16_t silence[256 * 2] = {};
  size_t written = 0;
  i2s_channel_write(s_tx, silence, sizeof silence, &written, pdMS_TO_TICKS(300));
  gpio_set_level((gpio_num_t)CPA_SD, 1);   // amp off
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_tx);
  s_tx = nullptr;
  xSemaphoreGive(s_mtx);
}
#endif  // HAS_CROWPANEL_P4_101
