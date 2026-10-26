// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A leaner Wi-Fi driver set-up on the T-LoRa Pager, so Bluetooth fits beside it.
//
// The Pager's internal RAM can't hold Wi-Fi, Bluetooth and the task stacks
// with Arduino's Wi-Fi settings: WiFi.mode() took 36 KB of it. Call
// wifiLeanInit() just before WiFi.mode(WIFI_STA). It initialises the driver
// first, with smaller settings; Arduino's own esp_wifi_init() then finds it
// done and returns (ESP-IDF 4.4 returns ESP_OK when already initialised).
// Arduino tears the driver down on WiFi.mode(WIFI_OFF), so every start goes
// through here again. Elsewhere it does nothing.
//
// What changes (Arduino 2.0.17's values in brackets):
//  - static RX buffers 3 [4]: each is 1.6 KB of internal DMA RAM, held for good.
//  - dynamic RX/TX buffer caps 16 [32]: limits on how many are in use at once.
//  - A-MPDU receive window 6 [16], A-MPDU transmit off [on]: the reorder state
//    and transmit aggregation sessions are internal RAM. This radio mostly
//    fetches small things (tiles, updates, the web mirror), so the throughput
//    they buy matters less than Bluetooth.
//  - management buffers 10 [32].
//  - no Wi-Fi NVS [on]: credentials live in our own store (WifiRuntimeStore),
//    and Arduino keeps the driver's copy in RAM anyway (WiFi.persistent(false)).
// Cache TX buffers stay at 4: those are PSRAM.

#if defined(ESP32) && defined(TLORA_PAGER)
#include <WiFi.h>
#include <esp_wifi.h>

bool tcpipInit();   // Arduino's WiFiGeneric.cpp: esp_netif + event loop, once

inline bool wifiLeanInit() {
  if (WiFi.getMode() != WIFI_MODE_NULL) return true;   // the driver is up already
  if (!tcpipInit()) return false;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.static_rx_buf_num  = 3;
  cfg.dynamic_rx_buf_num = 16;
  cfg.tx_buf_type        = 1;    // dynamic
  cfg.static_tx_buf_num  = 0;
  cfg.dynamic_tx_buf_num = 16;
  cfg.cache_tx_buf_num   = 4;    // PSRAM; can't be zero
  cfg.ampdu_rx_enable    = 1;
  cfg.rx_ba_win          = 6;
  cfg.ampdu_tx_enable    = 0;
  cfg.mgmt_sbuf_num      = 10;
  cfg.nvs_enable         = 0;
  const esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[wifi] lean init failed (%d); Arduino's defaults will be used\n", (int)err);
    return false;
  }
  return true;
}
#else
inline bool wifiLeanInit() { return true; }
#endif
