# The wadamesh app layer (MyMesh + DataStore + helpers + transports + the LVGL UITask),
# compiled directly from the repo's src/ — our own in-tree code (NOT vendored). Mirrors
# tanmatsu/components/wadamesh_app but builds variants/tdisplay_p4/ instead of variants/tanmatsu/,
# and — unlike the badge-bsp Tanmatsu — keeps the RAW-pin radio path (the T-Display P4 has a real
# SX1262 on P4 GPIOs). Excludes main.cpp (our app_main is in tdisplay_p4/main/) and the S3 input
# drivers (the P4's touch is HI8561, in variants/tdisplay_p4/).
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(APP "${REPO}/src")
# Also glob variants/tdisplay_p4/*.c — the vendored esp_lcd_hi8561.c (Espressif Apache-2.0, verbatim)
# is kept as C so it compiles without the C++ strictness patches esp_lcd_rm69a10.cpp needed; its header
# has extern "C", so HI8561Display.cpp (C++, LCD SKU) links against it fine.
file(GLOB_RECURSE APP_SRCS "${APP}/*.cpp" "${APP}/*.c" "${REPO}/variants/tdisplay_p4/*.cpp" "${REPO}/variants/tdisplay_p4/*.c")
list(FILTER APP_SRCS EXCLUDE REGEX "/main\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/helpers/input/(HeltecV4CapTouch|TDeckKeyboard|TDeckTouch|TDeckTrackball|RakTapV2Touch)\\.cpp$")

# Vendored Lua 5.4 VM (lib/lua) — the Lua app host (LUA_APPS.md). Plain C, RISC-V clean.
file(GLOB LUA_SRCS "${REPO}/lib/lua/src/*.c")

# Vendored Codec2 (lib/codec2, LGPL-2.1): plays MeshCore Open TAK voice messages and
# records them from the ES8311 mic (VoxVoice / VoxMic). Same flags as its library.json.
file(GLOB CODEC2_SRCS "${REPO}/lib/codec2/src/*.c")

# Vendored ggwave (lib/ggwave, MIT): Sound link's data over sound (SoundLinkUI.inc).
set(GGWAVE_SRCS "${REPO}/lib/ggwave/src/ggwave.cpp")

idf_component_register(
  SRCS ${APP_SRCS} ${LUA_SRCS} ${CODEC2_SRCS} ${GGWAVE_SRCS}
  INCLUDE_DIRS "${APP}" "${APP}/ui-touch" "${REPO}/include" "${REPO}/variants/tdisplay_p4" "${REPO}/lib/lua/src"
               "${REPO}/lib/codec2/include" "${REPO}/lib/ggwave/include"
  REQUIRES espressif__arduino-esp32 meshcore lvgl ardlibs RadioLib nvs_flash esp_lcd fatfs
           esp_driver_i2c esp_driver_gpio esp_hosted h2zero__esp-nimble-cpp bt
           soc esp_hw_support app_update driver esp_timer esp_partition spi_flash esp_adc esp_pm espcoredump
           c6_at   # C6WifiShim facade (WiFi.* -> AT-over-SDIO; UITask + WifiRuntimeStore include it)
           esp_http_client esp-tls mbedtls   # fork updates over HTTPS (ForkUpdate.inc: Arduino's TLS client doesn't link here)
  # See the Tanmatsu note: whole-archive so the strong ssl_client stub defs are always present.
  WHOLE_ARCHIVE
)
# Our own code, but huge + LVGL-heavy — keep the first port building; don't -Werror on warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -w
  -include "${REPO}/variants/tdisplay_p4/tdisplay_p4_compat.h")   # adcAttachPin() shim (arduino 3.x dropped it)

# Codec2's own flags (as its library.json). After idf_component_register: the
# requirements scan runs this file as a script, where these commands don't exist.
set_source_files_properties(${CODEC2_SRCS} PROPERTIES
  COMPILE_DEFINITIONS "__EMBEDDED__;CODEC2_MODE_700C_EN=0"
  COMPILE_OPTIONS "-Os")   # well ahead of real time at -Os on a 360 MHz P4

# ggwave's own flags (as its library.json).
set_source_files_properties(${GGWAVE_SRCS} PROPERTIES
  COMPILE_DEFINITIONS "GGWAVE_MAX_RECORDED_FRAMES=640"
  COMPILE_OPTIONS "-Os")

# The UI logic (UITask.cpp, the biggest file by far) at -Os: Codec2 left the image
# ~100 KB over its 4 MB OTA slot at -O2. The pixel path the #167 underrun fix is
# about (LVGL rendering, the DSI flush) is in other components and stays -O2.
set_source_files_properties("${APP}/ui-touch/UITask.cpp" PROPERTIES COMPILE_OPTIONS "-Os")

# The rest of our own app layer (src/: MyMesh, helpers, the other screens) at -Os
# too: the image stood ~200 B under its 4 MB slot and every feature tipped it
# over. None of it is the pixel path (LVGL rendering and the DSI flush are in the
# lvgl component and variants/tdisplay_p4/, which stay -O2), nor the radio driver
# (RadioLib, the meshcore component).
set(APP_OS_SRCS ${APP_SRCS})
list(FILTER APP_OS_SRCS EXCLUDE REGEX "/variants/")   # APP_SRCS is src/ plus variants/tdisplay_p4/
set_source_files_properties(${APP_OS_SRCS} PROPERTIES COMPILE_OPTIONS "-Os")
