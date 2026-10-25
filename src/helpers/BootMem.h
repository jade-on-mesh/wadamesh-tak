#pragma once
// Internal RAM at steps of start-up, kept for Settings > Device > Diagnostics >
// Memory detail, so a board that runs out (the Pager with Wi-Fi and BLE up)
// shows which step took it without a serial cable.
#include <stdint.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace bootmem {

struct Mark {
  const char* what;    // a string literal
  uint32_t free;       // internal free, bytes
  uint32_t largest;    // its largest block
};

constexpr int kMax = 10;
inline Mark g_marks[kMax];
inline int g_count = 0;

inline void mark(const char* what) {
#if defined(ESP32)
  if (g_count >= kMax) return;
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  g_marks[g_count++] = {what, (uint32_t)heap_caps_get_free_size(caps),
                        (uint32_t)heap_caps_get_largest_free_block(caps)};
#else
  (void)what;
#endif
}

}  // namespace bootmem
