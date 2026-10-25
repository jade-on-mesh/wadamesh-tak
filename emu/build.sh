#!/usr/bin/env bash
# Radio emulator tests: the real RadioLib SX126x driver, the meshcomod radio
# wrapper and Wadamesh's MediaMesh radio, on a modelled SX1262 per node and a
# shared simulated air (emu/air). From MatterMesh OS's PC emulator (see
# emu/README.md).
#
#   emu/build.sh          build and run every emulator test
#   emu/build.sh build    build only (emu/build/)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
emu/fetch-deps.sh
B=emu/build
mkdir -p "$B"
RL=emu/third_party/RadioLib/src
MC=emu/third_party/meshcomod/src
cat > "$B/Makefile" <<'MAKE'
.DEFAULT_GOAL := all
RL := emu/third_party/RadioLib/src
MC := emu/third_party/meshcomod/src
FLAGS := -std=gnu++17 -g -O1 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -MMD -MP
DEFS := -DARDUINO=10812 -DEMU_HOST=1 -DESP32 -DRADIOLIB_STATIC_ONLY=1 -DRADIOLIB_GODMODE=1 -DUSE_SX1262 \
 -DLORA_FREQ=910.525 -DLORA_BW=62.5 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=22 \
 -DSX126X_DIO3_TCXO_VOLTAGE=1.8 -DSX126X_RX_BOOSTED_GAIN=1 -DSX126X_DIO2_AS_RF_SWITCH=0
INCS := -Iemu/include -Iemu -Isrc -I$(MC) -I$(RL)
RADIO := emu/devices/sx1262.cpp emu/air/air_client.cpp $(wildcard emu/core/*.cpp) \
 $(MC)/helpers/radiolib/RadioLibWrappers.cpp \
 $(RL)/Module.cpp $(RL)/Hal.cpp $(RL)/hal/Arduino/ArduinoHal.cpp \
 $(RL)/protocols/PhysicalLayer/PhysicalLayer.cpp $(wildcard $(RL)/modules/SX126x/*.cpp) \
 $(wildcard $(RL)/utils/*.cpp)
TESTS := test_mediamesh_radio
all: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/emu-air
define test_rule
$(BUILD)/$(1): $(addprefix $(BUILD)/,$(patsubst %.cpp,%.o,emu/tests/$(1).cpp $(RADIO)))
	$$(CXX) $$(FLAGS) $$^ -o $$@
endef
$(foreach t,$(TESTS),$(eval $(call test_rule,$(t))))
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CXX) $(FLAGS) $(if $(filter $(RL)/Module.cpp,$<),-fno-sanitize=nonnull-attribute) $(DEFS) $(INCS) -w -c $< -o $@
$(BUILD)/emu-air: emu/air/air_broker.cpp emu/air/air_protocol.h
	$(CXX) $(FLAGS) -Wall -Wextra -Werror $< -o $@
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
MAKE
make --no-print-directory -f "$B/Makefile" -j"${EMU_JOBS:-4}" BUILD="$B" CXX="${CXX:-g++}"
[ "${1:-}" = build ] && exit 0
# LeakSanitizer can't inspect threads under the process tracer; ASan/UBSan stay on.
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
for t in "$B"/test_*; do
  echo "== $(basename "$t")"
  "$t" "$B/emu-air" </dev/null
done
