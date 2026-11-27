// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included on Arduino core 3 / ESP-IDF 5 builds (scripts/build/idf5_core_compat.py).
// Headers IDF 4 pulled in by itself and IDF 5 moved out; harmless where already included.
#pragma once
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_mac.h>   // esp_efuse_mac_get_default, esp_read_mac (was via esp_system.h)
#endif
