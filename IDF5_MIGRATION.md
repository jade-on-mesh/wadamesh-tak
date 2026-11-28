# Wadamesh TAK: moving the S3 standalones to ESP-IDF 5

Plan, 27 Nov 2026. Covers the 12 ESP32-S3 builds. The P4 and Tanmatsu are already on IDF 5.

## Where things stand

- **Toolchain.** Every S3 build uses `platformio/espressif32@6.11.0`, which is Arduino core 2.0.17 on ESP-IDF 4.4.7. The T-Display P4 and Tanmatsu builds use ESP-IDF 5.5.1, with Arduino 3.3.10 as an IDF component.
- **Shared code.** All of `src/`, UITask included, already compiles on IDF 5 for the P4. What has never been compiled on IDF 5 is the S3-only code: board `#if` blocks and `variants/`.
- **App flash is nearly full.** From the PR #276 CI run:

| Build | App flash used | Free |
|---|---|---|
| T-Deck | 98.0% of 4.625 MB | 98 KB |
| ThinkNode M9 | 97.4% | 126 KB |
| Heltec V4-R8 | 97.4% | 124 KB |
| Heltec V4 | 97.8% | 109 KB |
| T-LoRa Pager | 96.6% of 4.875 MB | 174 KB |
| Wio Tracker L2 | 99.0% of 4.375 MB | 45 KB |
| CrowPanel 7" | 97.3% of 4.375 MB | 123 KB |

- **Internal RAM on the T-Deck** (from the CI RAM report). There are 236 KB of static internal RAM:
  - 114 KB of .bss
  - 92 KB of code in IRAM
  - 28 KB of .data

  On the S3, IRAM and DRAM come out of the same internal SRAM, so code kept in IRAM is heap we don't get. The framework's own share includes:

  | Part | Internal RAM |
  |---|---|
  | FreeRTOS | 24 KB |
  | spi_flash | 16 KB |
  | Bluetooth controller | 13 KB |
  | esp_hw_support | 11 KB |
  | Wi-Fi (net80211) | 11 KB |
  | PHY | 8 KB |
  | HAL | 8 KB |
  | Coredump | 6 KB |
  | Ring buffer | 4 KB |

  On IDF 4 these are fixed, because Arduino ships them precompiled.
- **Already moved to PSRAM on IDF 4:**
  - Wi-Fi and LWIP buffers (Arduino's default).
  - TLS (`TlsPsramAlloc.h`, `ForkUpdate.inc`).
  - NimBLE's host pools, on the two Pager builds only (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`).

## What IDF 5 gets us

The move is worth it because, with pioarduino's `custom_sdkconfig`, we choose the framework's settings ourselves.

1. **Framework code out of IRAM.** These options move code that now sits in internal RAM out to flash:
   - `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`
   - `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH`
   - `CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`
   - `CONFIG_LWIP_IRAM_OPTIMIZATION=n`
   - Coredump off or to flash

   Estimate: **25 to 40 KB of internal heap back**. That is more than the 32 KB block the photo decoder kept failing to find.
2. **Task stacks in PSRAM.** `xTaskCreateWithCaps` (IDF 5.3+) can put stacks in PSRAM, including the AV1 decoder's 24 KB stack and the worker tasks. They must be tasks that never write flash.
3. **Statics in PSRAM by attribute.** `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` with `EXT_RAM_BSS_ATTR` would replace the hand-rolled `psramStatic<>()` calls.
4. **Log strings stripped** (`CONFIG_LOG_MAXIMUM_LEVEL`), and unused components switched off. This saves flash, which we need (see risk 1).
5. **One Arduino core on every board.** The Arduino-3 code paths the P4 already uses would serve the S3 boards too.

What we probably shouldn't copy from MESHtop:

- **XIP from PSRAM.** It copies the whole app image into PSRAM: about 4.7 MB of the T-Deck's 8 MB, leaving about 3.3 MB for tiles, emoji, photos and predictive text. The Heltec V4 has only 2 MB. Worth one measurement on the T-Deck, not a plan.

## Do now, on IDF 4

- **NimBLE pools in PSRAM on the other PSRAM boards**: T-Deck, M9, V4-R8, Wio L2, the CrowPanels and the SenseCAP. It's the same one-line build flag that fixed the Pager's "RAM 2K/1K" with Wi-Fi and BLE both up. It's independent of the migration.

## Risks

1. **Flash.** Arduino 3 builds come out bigger, and we start at 96 to 99% full (45 KB free on the Wio L2). The spike measures this before anything else.
   - Ways to claw it back: strip log strings, switch off unused components, use `-Os`.
   - If a board still won't fit: another repartition (one USB install, as before) or a feature off on that board.
2. **Displays.**
   - TFT_eSPI (Heltec V4, both Pagers, Attaky, T-Watch S3) has known trouble on core 3 with the S3. It needs checking early; the fallback is LovyanGFX, which is fine on core 3.
   - LovyanGFX boards (T-Deck Pro and Max, CrowPanels, Wio L2, V4-R8, SenseCAP) should be fine.
3. **Audio.** These use the legacy I2S driver (`driver/i2s.h`):
   - T-Deck speaker
   - Pager and T-Deck Max ES8311
   - Wio L2
   - `VoxMic`
   - `BuzzerVoice`

   IDF 5.5 still has the legacy driver, but it aborts at boot if the new one is linked too. Port them to `i2s_std` / `i2s_pdm`; the P4's ES8311 code is the template. `ledcSetup`/`ledcAttachPin` (T-Deck and M9 backlight, M9 buzzer, Pager keyboard, T-Deck Pro frontlight) becomes `ledcAttach`. `esp_task_wdt_init` takes a config struct.
4. **BLE.** NimBLE-Arduino 1.4.3 has to move to 2.x, which has a new API.
   - The P4's port of `SerialBLEInterface` to esp-nimble-cpp 2.x is nearly the same API and is the template.
   - The companion link to the app is part of the gold standard: pairing, existing bonds and throughput must come through unchanged.
5. **Over-the-air across the jump.** An update keeps the old IDF 4 bootloader. Prove that an IDF 5 app boots on it (flash and PSRAM modes) on the bench before offering it over the air. Otherwise the jump is a USB install.
6. **Saved data.** NVS and FAT formats carry over between IDF 4 and 5. Still verify that contacts, settings, history and bonds survive.
7. **Upstream sync.** Upstream Wadamesh stays on 6.11 and the fork syncs nightly. To keep those merges clean:
   - Keep the IDF 5 environments in a separate `platformio_idf5.ini` (`extra_configs`).
   - Put ported code behind `ESP_ARDUINO_VERSION_MAJOR >= 3`.
8. **CI time.** `custom_sdkconfig` rebuilds the Arduino libraries for each board, adding minutes per job. Cache them.

## Spike results (27 Nov 2026, T-Deck, pioarduino 55.03.311 = Arduino 3.3.11 / IDF 5.5.5)

**It builds.** All ~900 files compile. Five small fixes, all inert on 6.11:

| Fix | What it does |
|---|---|
| `LedcCompat.h` | Keeps LEDC addressed by channel on both cores. |
| IDF 5 watchdog call | Uses `esp_task_wdt_reconfigure` on IDF 5. |
| `esp_mac.h` force-include | Supplies `esp_efuse_mac_get_default`, which IDF 5 moved. |
| The core's ESP-NOW radio left out | It uses IDF 4 callback signatures, and no board uses it. |
| exFAT FatFs skipped | It needs re-vendoring from IDF 5.5. |

The spike environment also needs four settings: `-D ESP32`, `lib_archive = yes`, the framework's `SD` library named explicitly, and `-Wno-error=return-type`.

**With stock settings, it is worse on both counts:**

| T-Deck | 6.11 (today) | IDF 5, stock |
|---|---|---|
| firmware.bin | 4,751,776 B | **4,873,120 B** |
| App slot (4.625 MB) | fits, 98 KB free | **23 KB too big** |
| Code in IRAM (.iram0.text) | 91,975 B | 107,343 B |
| Static internal RAM (iram0 + dram0) | 235,564 B | 251,939 B (**+16 KB**) |

PlatformIO's "Flash 96.9%" line undercounts on IDF 5. The .bin size is what has to fit the slot. The IDF 5 figure doesn't include exFAT yet.

**Conclusion.** A plain switch buys nothing; it costs flash and internal RAM. The gains need the tuned framework settings. pioarduino's `custom_sdkconfig` can't apply them to this project: it turns the build into Arduino-as-an-IDF-component, which ignores `build_src_filter` and the library finder, and it failed on 55.03.311. So the next step is to build Arduino 3.3.11's libraries with **esp32-arduino-lib-builder** and our sdkconfig, and point `platform_packages` at them:

- IRAM off for FreeRTOS, the ring buffer, heap, Wi-Fi and LWIP.
- Log level stripped.
- Unused components off.

Then measure again. Go only if the bin fits the slot and static internal RAM comes in under today's 235 KB.

## Phases

0. **IDF 4 quick win.** NimBLE pools in PSRAM, as above. Small PR, bench check of BLE companion.
1. **Spike, T-Deck only.**
   - A `LilyGo_TDeck_idf5` environment on pioarduino `55.03.311` (Arduino 3.3.11, IDF 5.5.5), as a non-blocking CI job.
   - Fix compile errors behind version guards; nothing changes for released builds.
   - Output: does it build, its app size, and its static internal RAM against today. **Go / no-go.**
2. **Port.** LEDC, I2S, the watchdog, NimBLE 2.x, the TLS bundle API and Wi-Fi events, still with 6.11 as the release toolchain.
3. **Tune and measure on the T-Deck.** Apply the IRAM and PSRAM sdkconfig settings, then compare with today:
   - Free internal heap and largest block at idle, with Wi-Fi and BLE on (Settings > About).
   - Photos on a busy T-Deck.
   - The update check over Wi-Fi.
   - The emoji download.
4. **Bench matrix on the T-Deck.**
   - LoRa send and receive against IDF 4 standalones and the app.
   - BLE companion.
   - Wi-Fi web console.
   - Voice and photos.
   - SD, GPS, keyboard and trackball.
   - Sleep current.
   - Over-the-air from the current release and back.
5. **Roll out on the beta channel.**
   1. T-Deck first.
   2. Then the M9, the Pagers, V4-R8, Wio L2 and the CrowPanels.
   3. The 2 MB V4 and the TFT_eSPI boards last.
   4. Keep the 6.11 environments for one release as the way back.
6. **Align the P4 and Tanmatsu** to the same Arduino core.

## Decisions for Jade

- **Pilot board:** T-Deck is recommended (most-used, tightest on internal RAM).
- **If the IDF 5 T-Deck build doesn't fit 4.625 MB:** another repartition, or trim features?
- **The jump itself:** over the air if the old bootloader proves fine, or a USB install for everyone?
