// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh radio: the board's SX126x/LR11x0/LR2021 wrapper, extended to time-share the one
// transceiver between the primary profile and a second (`radio2`) profile.
// Variants declare `RADIO_DRIVER_CLASS radio_driver`, which is this class on
// supported boards and the plain wrapper everywhere else.
//
//  * A scan task on core 0 alternates the receiver between the profiles on the
//    schedule from MediaMeshCore.h. It never leaves a profile while a packet is
//    being received there, is waiting to be lifted out, or is being sent.
//  * Each packet's profiles come from the classifier (MediaMesh.cpp): a mask,
//    bit 0 the primary, bit 1 radio2. Both bits: the packet is sent twice, once
//    on each profile, before the dispatcher sees it complete. No bits: the send
//    fails (a `radio2` choice while radio2 can't transmit).
//  * Every received packet is reported with the profile it arrived on.
//  * Retunes are fast: the oscillator stays warm (XOSC standby) while scanning,
//    SF/BW/CR go to the chip in one SetModulationParams, and the preamble rides
//    on the packet parameters the receive/transmit start writes anyway
//    (mikecarper/MeshCore measured ~3.2 -> ~1.2 ms per hop for the same pair).
//  * The primary profile can be replaced for a while (tempradio) without
//    touching the saved radio settings.
//  * All radio access goes through the wrapper's recursive radio mutex, which
//    exists once buffered receive is on. Buffered receive is switched on while
//    radio2 is active: its drain task lifts a packet out of the radio within
//    ~1 ms, so the scan can move on.
//
// Requires an ESP32 with an SX126x, an LR1110/LR1121 (LR11x0: ThinkNode M9,
// T-LoRa Pager LR1121) or an LR2021 (T-Display P4 LR2021 build); elsewhere
// MEDIAMESH_RADIO stays undefined and the board keeps its stock wrapper. The
// LR chips retune through RadioLib's setters and arm the stock way (no lean
// arm); the scan measures their retunes and sizes its visits to them, as it
// does on the SX1262. A board whose DIO1 reaches no GPIO
// (MESH_RADIO_DIO1_POLLED: the T-Display P4, where it sits on an I2C expander)
// works too: the scan reads the chip's IRQ register over SPI anyway, so it
// stands in for the interrupt itself. A finished packet it finds wakes the
// drain task (pollRxIfNoIrq), and TX-done is read from the chip
// (isSendComplete), where the stock polled path only ever timed sends out.

#pragma once

#include "MediaMeshCore.h"

#if defined(ESP32) && !defined(MEDIAMESH_DISABLE) && (defined(USE_SX1262) || defined(USE_SX1268))
#define MEDIAMESH_CHIP_SX126X 1
#elif defined(ESP32) && !defined(MEDIAMESH_DISABLE) && defined(USE_LR2021)
#define MEDIAMESH_CHIP_LR2021 1
#elif defined(ESP32) && !defined(MEDIAMESH_DISABLE) && (defined(USE_LR1110) || defined(USE_LR1121) || defined(MEDIAMESH_CHIP_LR11X0))
#undef MEDIAMESH_CHIP_LR11X0
#define MEDIAMESH_CHIP_LR11X0 1
#endif

#if defined(MEDIAMESH_CHIP_SX126X) || defined(MEDIAMESH_CHIP_LR11X0) || defined(MEDIAMESH_CHIP_LR2021)

#define MEDIAMESH_RADIO 1

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if defined(MEDIAMESH_CHIP_SX126X)
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#endif

namespace mediamesh {
// The chip's own IRQ bits, standby modes and endless-receive timeout.
#if defined(MEDIAMESH_CHIP_SX126X)
using ScanChip = CustomSX1262;
static constexpr uint32_t kIrqRxDone = RADIOLIB_SX126X_IRQ_RX_DONE;
static constexpr uint32_t kIrqTxDone = RADIOLIB_SX126X_IRQ_TX_DONE;
static constexpr uint32_t kIrqHeader = RADIOLIB_SX126X_IRQ_HEADER_VALID;
static constexpr uint32_t kIrqPreamble = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
#elif defined(MEDIAMESH_CHIP_LR2021)
using ScanChip = RADIO_CLASS;
static constexpr uint32_t kIrqRxDone = RADIOLIB_LR2021_IRQ_RX_DONE;
static constexpr uint32_t kIrqTxDone = RADIOLIB_LR2021_IRQ_TX_DONE;
static constexpr uint32_t kIrqHeader = RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID;
static constexpr uint32_t kIrqPreamble = RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED;
static constexpr uint8_t kStandbyRc = RADIOLIB_LR2021_STANDBY_RC;
static constexpr uint8_t kStandbyXosc = RADIOLIB_LR2021_STANDBY_XOSC;
static constexpr uint32_t kRxForever = RADIOLIB_LR2021_RX_TIMEOUT_INF;
#else   // LR11x0
using ScanChip = RADIO_CLASS;
static constexpr uint32_t kIrqRxDone = RADIOLIB_LR11X0_IRQ_RX_DONE;
static constexpr uint32_t kIrqTxDone = RADIOLIB_LR11X0_IRQ_TX_DONE;
static constexpr uint32_t kIrqHeader = RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID;
static constexpr uint32_t kIrqPreamble = RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED;
static constexpr uint8_t kStandbyRc = 0x00;
// SetStandby's StbyConfig: 1 keeps the crystal/TCXO running (LR1110 user
// manual). RadioLib's RADIOLIB_LR11X0_STANDBY_XOSC reads 0, the RC value.
static constexpr uint8_t kStandbyXosc = 0x01;
static constexpr uint32_t kRxForever = RADIOLIB_LR11X0_RX_TIMEOUT_INF;
#endif
}  // namespace mediamesh

// The profiles a packet about to be sent goes out on (bit 0 primary, bit 1 radio2).
using MediaMeshClassifier = uint8_t (*)(const uint8_t* raw, int len);
// The same for the packet the dispatcher is about to send, before it is handed
// over (its airtime estimate); 0 when not known.
using MediaMeshPeek = uint8_t (*)();
// A packet was received on `origin` (0 primary, 1 radio2).
using MediaMeshOriginSink = void (*)(const uint8_t* raw, int len, uint8_t origin);

template <class Base>
class MediaMeshRadio : public Base {
  using Params = mediamesh::Params;

  Params _main_saved;               // the primary as the radio settings have it
  Params _main;                     // the primary in use (tempradio, or saved)
  bool _main_temp = false;
  bool _main_known = false;
  mediamesh::Profile2 _cfg;         // the second profile in use
  mediamesh::Timing _timing;
  volatile bool _scan = false;      // radio2 active: scan both profiles
  volatile bool _tx = false;        // a transmit is in progress
  volatile bool _paused = false;    // spectrum analyzer owns the radio
  // The chip is known to be in LoRa mode with the IQ register fix in place:
  // a full arm confirmed it, and only scan retunes have touched the radio
  // since. armScanLean() relies on it (see there).
  bool _lean_ok = false;
  volatile uint8_t _cur = 0;        // profile the radio is tuned to
  bool _user_rxq = false;           // buffered receive as the user set it
  uint32_t _hold_start = 0;
  mediamesh::SwitchTimes _switch_times;   // retunes since the last profile change
  uint32_t _switch_life_max_us = 0; // largest ever (radio2.status max=)
  uint8_t _switch_samples = 0;
  uint32_t _switches = 0;
  uint32_t _switch_errors = 0;
  uint32_t _switch_stalls = 0;      // retunes held up past kStallUs (not used for the timing)
  uint32_t _tx_count[2] = {};
  uint32_t _rx_count[2] = {};
  uint32_t _r2_refused = 0;         // radio2 sends refused: the scan wasn't running (or no timing)
  volatile uint32_t _scan_busy_us = 0;   // time the scan has held the radio (and its SPI bus)
  TaskHandle_t _task = nullptr;
  MediaMeshClassifier _classify = nullptr;
  MediaMeshPeek _peek = nullptr;
  MediaMeshOriginSink _origin_sink = nullptr;

  // The second copy of a packet sent on both profiles.
  uint8_t _copy_buf[256];
  int _copy_len = 0;
  uint8_t _copy_profile = 0xFF;
  bool _first_finished = false;

  // Which profile each drained packet arrived on, in arrival order. A packet
  // the drain task lifts out is counted in getPacketsRecv(); the scan never
  // leaves a profile with one still waiting, so every count since the last
  // retune belongs to the profile tuned now.
  static constexpr uint8_t kOriginSlots = 8;
  uint8_t _org[kOriginSlots];
  uint8_t _org_w = 0, _org_r = 0;
  uint32_t _org_seen = 0;

  mediamesh::ScanChip* chip() { return static_cast<mediamesh::ScanChip*>(this->_radio); }
#if !defined(MEDIAMESH_CHIP_SX126X)
  bool _warm = false;               // standby on the crystal while scanning
#endif

  uint16_t stockPreamble(uint8_t sf) const { return RadioLibWrapper::preambleLengthForSF(sf); }
  uint16_t mainPreamble() const { return _main.preamble ? _main.preamble : stockPreamble(_main.sf); }

  uint32_t switchBudgetUs() const {
    // Until a few hops are measured, assume a conservative allowance.
    if (_switch_samples < mediamesh::kSwitchSamples) return mediamesh::kDefaultSwitchUs;
    return (_switch_times.baseUs() * 11 + 9) / 10;
  }

  void resetSwitchTest() { _switch_samples = 0; _switch_times.reset(); }

  void recalc() {
    if (!_main_known || !mediamesh::validParams(_cfg.p)) { _timing = mediamesh::Timing(); return; }
    _timing = mediamesh::calcTiming(_main, mainPreamble(), _cfg.p, switchBudgetUs());
  }

#if defined(MEDIAMESH_CHIP_SX126X)
  static uint8_t bwCode(float bw) {
    static const float bws[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
    static const uint8_t codes[] = {RADIOLIB_SX126X_LORA_BW_7_8,   RADIOLIB_SX126X_LORA_BW_10_4,
                                    RADIOLIB_SX126X_LORA_BW_15_6,  RADIOLIB_SX126X_LORA_BW_20_8,
                                    RADIOLIB_SX126X_LORA_BW_31_25, RADIOLIB_SX126X_LORA_BW_41_7,
                                    RADIOLIB_SX126X_LORA_BW_62_5,  RADIOLIB_SX126X_LORA_BW_125_0,
                                    RADIOLIB_SX126X_LORA_BW_250_0, RADIOLIB_SX126X_LORA_BW_500_0};
    for (size_t i = 0; i < sizeof(bws) / sizeof(bws[0]); ++i) if (fabsf(bw - bws[i]) < 0.01f) return codes[i];
    return 0xFF;
  }
#endif

  // ---- receive origins (caller holds the radio mutex) ------------------------

  void pushOrigin(uint8_t o) {
    const uint8_t next = (uint8_t)((_org_w + 1) % kOriginSlots);
    if (next == _org_r) _org_r = (uint8_t)((_org_r + 1) % kOriginSlots);   // overrun: drop the oldest
    _org[_org_w] = o;
    _org_w = next;
  }
  void syncOrigins() {
    const uint32_t n = this->getPacketsRecv();
    while (_org_seen != n) { pushOrigin(_cur); ++_org_seen; }
  }
  // A packet completed in the radio but not yet lifted out by the drain task:
  // the chip's own RX-done flag, which the re-arm after the drain clears.
  bool rxWaiting() { return (chip()->getIrqFlags() & mediamesh::kIrqRxDone) != 0; }

  // Standby without the wake-up NOP (the chip is awake: it was receiving), on
  // the crystal while scanning so the next receive starts at once.
  void standbyQuick() {
#if defined(MEDIAMESH_CHIP_SX126X)
    chip()->standby(chip()->standbyXOSC ? RADIOLIB_SX126X_STANDBY_XOSC : RADIOLIB_SX126X_STANDBY_RC, false);
#else
    chip()->standby(_warm ? mediamesh::kStandbyXosc : mediamesh::kStandbyRc, false);
#endif
  }

  // ---- retuning (caller holds the radio mutex) -------------------------------

  // Puts the chip (already in standby: warm while scanning) on `p`: frequency,
  // then SF/BW/CR in one command. The preamble is written by the next
  // startReceive/startTransmit.
#if defined(MEDIAMESH_CHIP_SX126X)
  bool tune(const Params& p, uint16_t preamble) {
    CustomSX1262* r = chip();
    const uint8_t bwc = bwCode(p.bw);
    if (bwc == 0xFF) return false;
    int16_t rc = r->setFrequency(p.freq);   // image calibration only past 20 MHz
    if (rc == RADIOLIB_ERR_NONE
        && (r->spreadingFactor != p.sf || r->bandwidth != bwc || r->codingRate != (uint8_t)(p.cr - 4)
            || r->bandwidthKhz != p.bw)) {
      r->spreadingFactor = p.sf;
      r->bandwidth = bwc;
      r->bandwidthKhz = p.bw;
      r->codingRate = (uint8_t)(p.cr - 4);
      rc = r->setModulationParams(p.sf, bwc, (uint8_t)(p.cr - 4), r->ldrOptimize);
    }
    r->preambleLengthLoRa = preamble;
    PacketMillis pm = this->calcMaxPacketMillis(p.sf, p.bw, p.cr, preamble > 255 ? 255 : (uint8_t)preamble);
    r->setPreambleMillis(pm.preambleMillis);
    r->setMaxPayloadMillis(pm.payloadMillis);
    return rc == RADIOLIB_ERR_NONE;
  }
#else
  // LR11x0/LR2021: the frequency (an image calibration only for a jump of
  // many MHz, not between two channels of one band), then SF/BW/CR through
  // RadioLib's setters when they differ. The preamble goes out with the
  // packet parameters the receive/transmit start writes.
  bool tune(const Params& p, uint16_t preamble) {
    mediamesh::ScanChip* r = chip();
    int16_t rc = r->setFrequency(p.freq);
    if (rc == RADIOLIB_ERR_NONE
        && (r->spreadingFactor != p.sf || fabsf(r->bandwidthKhz - p.bw) > 0.01f
            || r->codingRate != (uint8_t)(p.cr - 4))) {
      rc = r->setBandwidth(p.bw);
      if (rc == RADIOLIB_ERR_NONE) rc = r->setSpreadingFactor(p.sf);
      if (rc == RADIOLIB_ERR_NONE) rc = r->setCodingRate(p.cr);
    }
    r->preambleLengthLoRa = preamble;
    PacketMillis pm = this->calcMaxPacketMillis(p.sf, p.bw, p.cr, preamble > 255 ? 255 : (uint8_t)preamble);
    r->setPreambleMillis(pm.preambleMillis);
    r->setMaxPayloadMillis(pm.payloadMillis);
    return rc == RADIOLIB_ERR_NONE;
  }
#endif

  // `scanning`: a retune between two receive visits. The chip goes to standby
  // without the wrapper's idle(), so its state stays "receiving" and the next
  // startRecvScan() arms the receiver once instead of twice (the wrapper's
  // stock arm and then ours with the preamble flag): about half the SPI
  // commands per retune, and on the T-Deck every command is a chance to wait
  // for the screen. If a packet finished just before the standby, the stock
  // path is taken, as before. Returns whether the fast path was taken.
  bool applyProfile(uint8_t profile, bool scanning = false) {
    syncOrigins();
    bool fast = false;
    if (scanning && this->isInRecvMode()) {
      // From receive the chip is awake: no wake-up NOP before the standby.
      standbyQuick();
      fast = !rxWaiting();   // in standby nothing new can land
      if (!fast) this->idle();
    } else {
      this->idle();
    }
    if (!fast) _lean_ok = false;
    const bool ok = profile ? tune(_cfg.p, _timing.valid ? _timing.preamble[1] : mediamesh::kMaxAutoPreamble)
                            : tune(_main, mainPreamble());
    if (!ok) {
      // Fall back to the stock path, which also resynchronises RadioLib's caches.
      ++_switch_errors;
      const Params& p = profile ? _cfg.p : _main;
      Base::setParams(p.freq, p.bw, p.sf, p.cr);
      chip()->setPreambleLength(profile ? (_timing.valid ? _timing.preamble[1] : mediamesh::kMaxAutoPreamble)
                                        : mainPreamble());
    }
    _cur = profile;
    return fast && ok;   // the stock fallback leaves the wrapper idle
  }

  // A retune slower than this was held up waiting for the SPI bus (on the
  // T-Deck the screen shares it with the radio and a redraw can hold it for
  // tens of ms), not a measure of the radio. Taking it as the per-switch
  // budget would make every cycle assume it: the radio2 visit shrinks to its
  // minimum, radio2's preamble goes to its cap and main gets the warning, for
  // good. It is counted (stalls) and left out of the timing instead.
  static constexpr uint32_t kStallUs = 8000;

  void sampleSwitch(uint32_t us) {
    if (us > _switch_life_max_us) _switch_life_max_us = us;
    if (us >= kStallUs) {
      ++_switch_stalls;
      return;
    }
    const uint32_t before = switchBudgetUs();
    if (_switch_samples < 255) ++_switch_samples;
    _switch_times.add(us);
    if (_switch_samples >= mediamesh::kSwitchSamples && switchBudgetUs() != before) recalc();
  }

  // Receive, with the preamble-detected flag latched while scanning. RadioLib's
  // default receive IRQ set (what RadioLibWrapper::startRecv asks for) leaves
  // it out, so isReceivingPacket() only turned true at HEADER_VALID: after the
  // whole preamble and the header, ~12 symbols past its end. A visit lasts
  // ~4.6 symbols, so the scan almost always moved on in the middle of a
  // preamble and the packet was lost: T-Decks with radio2 on received
  // nothing, on either profile. (Core's CustomLLCC68 asks for it the same
  // way.) Without radio2 the stock receive is kept: a latched preamble flag
  // nothing clears would read as "receiving" for good.
  //
  // `armed`: applyProfile's fast path left the wrapper "receiving", so only
  // our arm is needed. If it fails, the stock path puts things right.
  void startRecvScan(bool armed = false) {
    if (armed && _scan) {
      if (_lean_ok && armScanLean() == RADIOLIB_ERR_NONE) return;
      if (armScan() == RADIOLIB_ERR_NONE) { confirmLean(); return; }
      _lean_ok = false;
    }
    this->startRecv();   // the wrapper's state (receiving) and the stock arm
    if (!_scan) return;
    if (armScan() == RADIOLIB_ERR_NONE) confirmLean();
    else _lean_ok = false;
  }
  // After a full arm: the lean one may follow if the chip is in LoRa with
  // standard IQ (one packet-type query, on this path only).
  void confirmLean() {
#if defined(MEDIAMESH_CHIP_SX126X)
    CustomSX1262* r = chip();
    _lean_ok = r->invertIQEnabled == RADIOLIB_SX126X_LORA_IQ_STANDARD
            && r->getPacketType() == RADIOLIB_SX126X_PACKET_TYPE_LORA;
#else
    _lean_ok = false;   // the LR chips always take the full arm
#endif
  }

  // RadioLib's receive arm, less what a scan retune can't have changed. From
  // keymindCascade's T-Deck work (mikecarper/MeshCore d4c826322: "confirmed
  // modem" and "IQ workaround" caches, checked over thousands of retunes):
  //  - no standby (applyProfile just put the chip there), so no wake-up NOP;
  //  - no packet-type query: only a full arm sets _lean_ok, after confirming
  //    LoRa, and anything that could change the modem (a send, new radio
  //    settings, the spectrum analyzer) clears it;
  //  - no read-modify-write of the IQ register (SX126x errata 15.4): the full
  //    arm wrote it for standard IQ, which MeshCore always uses, and standby
  //    keeps registers.
  // What is left: IRQ masks, buffer base, clear IRQs, packet parameters (the
  // preamble differs between the profiles), RX. Eight SPI commands a retune
  // in all, where RadioLib's path made about fifteen.
  int16_t armScanLean() {
#if !defined(MEDIAMESH_CHIP_SX126X)
    return RADIOLIB_ERR_UNKNOWN;   // not used: confirmLean() never allows it here
#else
    CustomSX1262* r = chip();
    const uint16_t irqs = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT | RADIOLIB_SX126X_IRQ_CRC_ERR
                        | RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_HEADER_ERR
                        | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
    int16_t rc = r->setDioIrqParams(irqs, RADIOLIB_SX126X_IRQ_RX_DONE);
    if (rc == RADIOLIB_ERR_NONE) rc = r->setBufferBaseAddress();
    if (rc == RADIOLIB_ERR_NONE) rc = r->clearIrqStatus();
    if (rc == RADIOLIB_ERR_NONE) {
      const uint16_t pre = r->preambleLengthLoRa;
      const uint8_t data[6] = {(uint8_t)(pre >> 8), (uint8_t)(pre & 0xFF), r->headerType,
                               (uint8_t)r->implicitLen, r->crcTypeLoRa, r->invertIQEnabled};
      rc = r->mod->SPIwriteStream(RADIOLIB_SX126X_CMD_SET_PACKET_PARAMS, data, 6);
    }
    if (rc == RADIOLIB_ERR_NONE) {
      r->mod->setRfSwitchState(Module::MODE_RX);
      rc = r->setRx(RADIOLIB_SX126X_RX_TIMEOUT_INF);
    }
    return rc;
#endif
  }
  int16_t armScan() {
#if defined(MEDIAMESH_CHIP_SX126X)
    return chip()->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
                                RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                                RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
#else
    // Through PhysicalLayer: the Custom classes' startReceive() hides the
    // four-argument form.
    return static_cast<PhysicalLayer*>(chip())->startReceive(
        mediamesh::kRxForever, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
        RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
#endif
  }

  // How long a preamble with no header yet may keep the receiver on a profile:
  // the longest preamble its senders use, plus the header, plus a margin. Past
  // that it was noise (or a packet we joined too late) and the scan moves on.
  uint32_t preambleHoldMs(uint8_t profile) const {
    const Params& p = profile ? _cfg.p : _main;
    const uint16_t pre = profile ? (_timing.valid ? _timing.preamble[1] : mediamesh::kMaxAutoPreamble)
                                 : mainPreamble();
    const double sym_us = mediamesh::symbolUs(p.bw, p.sf);
    return (uint32_t)(((double)pre + 4.25 + 12.0) * sym_us / 1000.0) + 30;
  }

  // The longest a profile may hold the receiver for one packet before a stuck
  // preamble/header flag is treated as noise.
  uint32_t maxHoldMs(uint8_t profile) const {
    const Params& p = profile ? _cfg.p : _main;
    const uint16_t pre = profile ? _timing.preamble[1] : mainPreamble();
    return mediamesh::timeOnAirUs(p, pre, 255) / 1000 * 3 / 2 + 50;
  }

  uint32_t visitTicks(uint8_t profile) const {
    const uint32_t us = _timing.dwell_us[profile];
    // Round the slow visit up (it must cover 4.6 symbols) and the fast one down
    // (it must end inside half the slow preamble).
    uint32_t ms = profile == _timing.slow ? (us + 999) / 1000 : us / 1000;
    if (ms == 0) ms = 1;
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
  }

  static void scanTask(void* arg) { static_cast<MediaMeshRadio*>(arg)->scanLoop(); }

  void scanLoop() {
    for (;;) {
      if (!_scan || _paused || !_timing.valid) {
        // radio2 was switched off while a packet was going out on it: back to
        // the main profile once that send is done, or the radio would keep
        // listening on radio2's frequency.
        if (!_scan && !_paused) {
          this->radioAcquire();
          if (!_scan && !_tx && _cur != 0) {
            applyProfile(0);
            startRecvScan();
          }
          this->radioRelease();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        continue;
      }
      vTaskDelay(visitTicks(_cur));
      const uint32_t held0 = micros();
      this->radioAcquire();
      if (_scan && !_paused && !_tx && this->isInRecvMode()) {
        // Stay while a packet is arriving, and while a finished one waits for
        // the drain task (~1 ms), so every packet is read out on the profile it
        // came in on and credited to it.
        const uint32_t irq = chip()->getIrqFlags();
        const bool header = (irq & mediamesh::kIrqHeader) != 0;
        const bool preamble = (irq & mediamesh::kIrqPreamble) != 0;
        const bool waiting = (irq & mediamesh::kIrqRxDone) != 0;
#if defined(MESH_RADIO_DIO1_POLLED)
        // No DIO1 interrupt on this board: this read is it. Hand the packet to
        // the drain task now (it lifts it out within ~1 ms), instead of
        // leaving it for the main loop or the drain task's 250 ms safety tick.
        if (waiting) this->pollRxIfNoIrq();
#endif
        if (header || preamble || waiting) {
          const uint32_t now = millis();
          if (!_hold_start) _hold_start = now;
          // A header: the whole packet. A preamble alone: until a header
          // could have followed. A finished packet: the drain task (~1 ms).
          const uint32_t limit = header ? maxHoldMs(_cur) : preamble ? preambleHoldMs(_cur) : 50;
          if (now - _hold_start < limit) {
            this->radioRelease();
            _scan_busy_us += micros() - held0;
            vTaskDelay(1);
            continue;
          }
        }
        _hold_start = 0;
        const uint32_t t0 = micros();
        const bool armed = applyProfile(_cur ^ 1, true);
        startRecvScan(armed);
        sampleSwitch(micros() - t0);
        ++_switches;
      } else {
        _hold_start = 0;
      }
      this->radioRelease();
      _scan_busy_us += micros() - held0;
    }
  }

  void wake() { if (_task) xTaskNotifyGive(_task); }

  // A board without a DIO1 interrupt never sees TX-done through the wrapper:
  // read it from the chip. (The wrapper's state then stays "sending" until
  // onSendFinished, which every path below and the dispatcher call next.)
  bool txDonePolled() {
#if defined(MESH_RADIO_DIO1_POLLED)
    if (!_tx) return false;
    this->radioAcquire();
    const bool done = (chip()->getIrqFlags() & mediamesh::kIrqTxDone) != 0;
    this->radioRelease();
    return done;
#else
    return false;
#endif
  }

  // Sends on one profile. Caller does not hold the mutex.
  bool sendOn(uint8_t profile, const uint8_t* bytes, int len) {
    this->radioAcquire();
    _tx = true;
    _lean_ok = false;
    if (_cur != profile) applyProfile(profile);
    const bool ok = Base::startSendRaw(bytes, len);
    if (ok) ++_tx_count[profile];
    else _tx = false;
    this->radioRelease();
    return ok;
  }

#if defined(MEDIAMESH_CHIP_SX126X)
  void setWarmStandby(bool on) { chip()->standbyXOSC = on; }
  void restorePreamble() {}
#else
  // The LR wrappers' onSendFinished() put the preamble back to the stock one
  // for the SF tuned now. On radio2 that is shorter than the scan's: the next
  // radio2 send would go out with a preamble a scanning receiver can miss.
  void restorePreamble() {
    if (!_main_known) return;
    this->radioAcquire();
    chip()->preambleLengthLoRa = _cur ? (_timing.valid ? _timing.preamble[1] : mediamesh::kMaxAutoPreamble)
                                      : mainPreamble();
    this->radioRelease();
  }
  void setWarmStandby(bool on) { _warm = on; }
#endif

 public:
  using Base::Base;

  // ---- configuration ------------------------------------------------------

  void setMediaClassifier(MediaMeshClassifier fn, MediaMeshPeek peek = nullptr) { _classify = fn; _peek = peek; }
  void setOriginSink(MediaMeshOriginSink fn) { _origin_sink = fn; }

  // The second profile in use (saved or temporary); off stops the scan.
  void mediaMeshConfigure(const mediamesh::Profile2& cfg) {
    this->radioAcquire();
    _lean_ok = false;
    syncOrigins();
    const bool params_changed = !mediamesh::sameParams(cfg.p, _cfg.p) || cfg.mode != _cfg.mode;
    _cfg = cfg;
    const bool scan = mediamesh::scanning(cfg) && _main_known;
    if (params_changed) resetSwitchTest();
    recalc();
    setWarmStandby(scan);
    if (!scan && _cur != 0 && !_tx) {
      applyProfile(0);
      startRecvScan();
    } else if (scan && params_changed && _cur == 1 && !_tx) {
      applyProfile(1);   // retune to the new radio2 settings
      startRecvScan();
    }
    if (!scan && _cur == 0 && !_tx && params_changed) {
      // Back to one profile: let the stock path set the cold standby policy.
      Base::setParams(_main.freq, _main.bw, _main.sf, _main.cr);
      if (_main.preamble) chip()->setPreambleLength(_main.preamble);
      startRecvScan();
    }
    _scan = scan;
    this->radioRelease();
    Base::rxQueueEnable(_user_rxq || scan);
    if (scan && !_task) {
      xTaskCreatePinnedToCore(scanTask, "mediamesh", 3072, this, 9, &_task, 0);
    }
    wake();
  }

  // tempradio: replace the primary profile until called with nullptr.
  void setTempPrimary(const Params* p) {
    this->radioAcquire();
    syncOrigins();
    _main_temp = p != nullptr;
    _main = p ? *p : _main_saved;
    resetSwitchTest();
    recalc();
    if (!_tx) {
      Base::setParams(_main.freq, _main.bw, _main.sf, _main.cr);
      chip()->setPreambleLength(mainPreamble());
      PacketMillis pm = this->calcMaxPacketMillis(_main.sf, _main.bw, _main.cr,
                                                  mainPreamble() > 255 ? 255 : (uint8_t)mainPreamble());
      chip()->setPreambleMillis(pm.preambleMillis);
      chip()->setMaxPayloadMillis(pm.payloadMillis);
      _cur = 0;
        this->idle();
      startRecvScan();
    }
    this->radioRelease();
    wake();
  }

  const mediamesh::Profile2& mediaMeshProfile2() const { return _cfg; }
  const mediamesh::Timing& mediaMeshTiming() const { return _timing; }
  bool mediaMeshScanning() const { return _scan; }
  uint8_t mediaMeshProfile() const { return _cur; }
  uint32_t mediaMeshSwitches() const { return _switches; }
  uint32_t mediaMeshSwitchErrors() const { return _switch_errors; }
  uint32_t mediaMeshSwitchStalls() const { return _switch_stalls; }
  uint32_t mediaMeshSwitchUs() const { return _switch_samples ? _switch_times.baseUs() : 0; }
  uint32_t mediaMeshSwitchTypUs() const { return _switch_samples ? _switch_times.typicalUs() : 0; }
  uint32_t mediaMeshSwitchMaxUs() const { return _switch_life_max_us; }
  uint32_t mediaMeshSwitchBudgetUs() const { return switchBudgetUs(); }
  uint8_t mediaMeshSwitchSamples() const { return _switch_samples; }
  uint32_t mediaMeshTxCount(uint8_t profile) const { return _tx_count[profile & 1]; }
  uint32_t mediaMeshRxCount(uint8_t profile) const { return _rx_count[profile & 1]; }
  uint32_t mediaMeshRefused() const { return _r2_refused; }
  uint32_t mediaMeshScanBusyUs() const { return _scan_busy_us; }
  bool mediaMeshTimingValid() const { return _timing.valid; }
  const Params& mainParams() const { return _main; }
  const Params& mainSavedParams() const { return _main_saved; }
  uint16_t mainPreambleSymbols() const { return mainPreamble(); }
  bool mainTemporary() const { return _main_temp; }

  // ---- wrapper overrides --------------------------------------------------

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    this->radioAcquire();
    _lean_ok = false;
    _main_saved.freq = freq; _main_saved.bw = bw; _main_saved.sf = sf; _main_saved.cr = cr;
    _main_saved.preamble = 0;
    const bool first = !_main_known;
    _main_known = true;
    if (!_main_temp) {
      syncOrigins();
      _main = _main_saved;
      Base::setParams(freq, bw, sf, cr);
      _cur = 0;
      }
    if (!first) resetSwitchTest();
    recalc();
    if (first && mediamesh::scanning(_cfg)) { _scan = true; setWarmStandby(true); }
    this->radioRelease();
    wake();
  }

  int recvRaw(uint8_t* bytes, int sz) override {
    this->radioAcquire();
    syncOrigins();
    const uint8_t cur = _cur;
    this->radioRelease();
    const int len = Base::recvRaw(bytes, sz);
    if (len <= 0) return len;
    uint8_t origin = 0;
    this->radioAcquire();
    if (_org_r != _org_w) {
      origin = _org[_org_r];
      _org_r = (uint8_t)((_org_r + 1) % kOriginSlots);
    } else {
      origin = cur;                         // read straight from the radio (no drain task)
      _org_seen = this->getPacketsRecv();
    }
    this->radioRelease();
    if (!_scan) origin = 0;
    ++_rx_count[origin];
    if (_origin_sink) _origin_sink(bytes, len, origin);
    return len;
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    uint8_t mask = _classify ? _classify(bytes, len) : 1;
    if (!_scan || !_timing.valid) {
      if (mask & 2) ++_r2_refused;   // for radio2, but radio2 isn't running: radio2.status drop=
      mask &= 1;
    }
    if (!mask) return false;
    _first_finished = false;
    _copy_profile = 0xFF;
    uint8_t first = mask == 2 ? 1 : 0;
    if (mask == 3) {
      first = _cur;                         // whichever profile the radio is on now
      if (len > 0 && len <= (int)sizeof(_copy_buf)) {
        memcpy(_copy_buf, bytes, len);
        _copy_len = len;
        _copy_profile = first ^ 1;
      }
    }
    return sendOn(first, bytes, len);
  }

  bool isSendComplete() override {
    if (!Base::isSendComplete() && !txDonePolled()) return false;
    if (_copy_profile != 0xFF) {
      const uint8_t p = _copy_profile;
      _copy_profile = 0xFF;
      Base::onSendFinished();
      restorePreamble();
      _first_finished = true;
      if (sendOn(p, _copy_buf, _copy_len)) {
        _first_finished = false;
        return false;                       // the second copy is on air now
      }
      _tx = false;
      return true;                          // the first copy went; report that
    }
    return true;
  }

  void onSendFinished() override {
    if (!_first_finished) {
      Base::onSendFinished();
      restorePreamble();
    }
    _first_finished = false;
    _copy_profile = 0xFF;
    _tx = false;
    _hold_start = 0;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    if (!_scan || !_timing.valid) return Base::getEstAirtimeFor(len_bytes);
    const uint32_t a = mediamesh::timeOnAirUs(_main, mainPreamble(), len_bytes);
    const uint32_t b = mediamesh::timeOnAirUs(_cfg.p, _timing.preamble[1], len_bytes);
    // The packet about to go out is known: its own profile(s). Otherwise (the
    // dispatcher's budget check) the slower of the two.
    const uint8_t m = _peek ? _peek() : 0;
    if (m == 1) return a / 1000;
    if (m == 2) return b / 1000;
    if (m == 3) return (a + b) / 1000;   // both back to back (no packet free to copy)
    return (a > b ? a : b) / 1000;
  }

  void powerOff() override {
    _scan = false;
    Base::powerOff();
  }

  // Hides the wrapper's methods: buffered receive stays on while radio2 is
  // active, and the scan stops while the spectrum analyzer drives the radio.
  void rxQueueEnable(bool en) {
    _user_rxq = en;
    Base::rxQueueEnable(en || _scan);
  }
  void rxQueueSuspend(bool s) {
    this->radioAcquire();   // not mid-retune: the next arm after this is a full one
    _lean_ok = false;
    _paused = s;
    this->radioRelease();
    Base::rxQueueSuspend(s);
    if (!s) wake();
  }
};

#define RADIO_DRIVER_CLASS MediaMeshRadio<WRAPPER_CLASS>

#else

#define RADIO_DRIVER_CLASS WRAPPER_CLASS

#endif
