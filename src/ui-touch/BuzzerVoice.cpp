// SPDX-License-Identifier: GPL-3.0-or-later
// Voice through a buzzer (BoardAudioOut.h), for boards whose only sound is a buzzer
// on a GPIO: the ThinkNode M9 (GPIO9). (The Heltec V4's would work the same way, but
// its image has no room for the audio code: device_caps.h.) The ESP32-S3's I2S0
// sends the audio as 1-bit PDM on the buzzer's pin, a stream of pulses whose
// density follows the waveform; the buzzer and its driver smooth it into sound.
// DMA does the work, so playing costs next to no CPU. The pin is the buzzer's only
// while something plays: afterwards it goes back to high-Z for the beeps (tone()).
//
// A buzzer is loud only near its resonance (2.5-4 kHz on typical parts) and has no
// low end, so the voice is shaped for it: the lows are cut (they'd only make the
// part rattle) and what's left is lifted, then limited. It will sound thin and
// small, but words carry.
#include "device_caps.h"

#if defined(HAS_BOARD_AUDIO_OUT) && !defined(HAS_CROWPANEL_P4_101)
#include "BoardAudioOut.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <soc/soc_caps.h>

#if !SOC_I2S_SUPPORTS_PDM_TX
#error "BuzzerVoice needs I2S PDM TX (ESP32-S3)"
#endif

#if defined(THINKNODE_M9_BUZZER_PIN)
static constexpr int kPin = THINKNODE_M9_BUZZER_PIN;
#else
static constexpr int kPin = HELTEC_V4_BUZZER_PIN;
#endif
static constexpr i2s_port_t kPort = I2S_NUM_0;   // PDM TX is I2S0's alone
static constexpr uint32_t kRate = 16000;

static volatile bool s_open = false;
// Shaping state (one player at a time).
static float s_hp_x = 0, s_hp_y = 0;   // high-pass, ~300 Hz
static float s_pe_x = 0;               // pre-emphasis

uint32_t boardAudioStreamRate() { return kRate; }

bool boardAudioStreamBegin() {
  if (s_open) return false;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;   // an underrun sends silence, not the last buffer again
  noTone(kPin);                     // a beep's LEDC lets go of the pin
  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = I2S_PIN_NO_CHANGE;   // the PDM clock: a buzzer doesn't need it
  pins.data_out_num = kPin;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kPort);
    return false;
  }
  i2s_zero_dma_buffer(kPort);
  s_hp_x = s_hp_y = s_pe_x = 0;
  s_open = true;
  Serial.printf("[BUZVOICE] buzzer on GPIO%d as audio (PDM, %u Hz)\n", kPin, (unsigned)kRate);
  return true;
}

// One sample shaped for a buzzer: high-pass (~300 Hz at 16 kHz), pre-emphasis
// toward the 2.5-4 kHz the part plays loudest, twice the level, soft limit.
static inline int16_t shape(int16_t in) {
  const float x = (float)in;
  const float hp = 0.889f * (s_hp_y + x - s_hp_x);
  s_hp_x = x;
  s_hp_y = hp;
  float y = (hp - 0.6f * s_pe_x) * 2.0f;
  s_pe_x = hp;
  const float lim = 30000.0f;
  if (y > lim || y < -lim) y = (y > 0 ? lim : -lim);
  return (int16_t)y;
}

bool boardAudioStreamWrite(const int16_t* samples, size_t frames) {
  if (!s_open || !samples || !frames) return false;
  int16_t buf[256];
  while (frames) {
    const size_t count = frames > 256 ? 256 : frames;
    for (size_t i = 0; i < count; ++i) buf[i] = shape(samples[i]);
    size_t written = 0;
    const size_t bytes = count * sizeof(int16_t);
    if (i2s_write(kPort, buf, bytes, &written, pdMS_TO_TICKS(300)) != ESP_OK || written != bytes) return false;
    samples += count;
    frames -= count;
  }
  return true;
}

void boardAudioStreamEnd() {
  if (!s_open) return;
  static const int16_t silence[256] = {};
  size_t written = 0;
  i2s_write(kPort, silence, sizeof silence, &written, pdMS_TO_TICKS(300));
  i2s_zero_dma_buffer(kPort);
  i2s_driver_uninstall(kPort);
  // Back to the beeps: off the I2S signal, high-Z (no idle current, no buzz).
  gpio_reset_pin((gpio_num_t)kPin);
  pinMode(kPin, INPUT);
  s_open = false;
}
#endif
