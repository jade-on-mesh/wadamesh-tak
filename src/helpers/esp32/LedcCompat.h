// SPDX-License-Identifier: GPL-3.0-or-later
// LEDC by channel on both Arduino cores (IDF5_MIGRATION.md).
//
// Core 2 (ESP-IDF 4) drives LEDC by channel: ledcSetup + ledcAttachPin, then
// ledcWrite(channel, duty). Core 3 (ESP-IDF 5) drives it by PIN: ledcWrite(pin, duty)
// keeps the old name and signature, so an unported call would compile and write the
// wrong thing. Callers use these instead; on core 2 they are exactly the old calls.
#pragma once

#include <Arduino.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
inline void wmLedcAttach(uint8_t ch, uint8_t pin, uint32_t hz, uint8_t bits) { ledcAttachChannel(pin, hz, bits, ch); }
inline void wmLedcWrite(uint8_t ch, uint32_t duty) { ledcWriteChannel(ch, duty); }
inline void wmLedcFrequency(uint8_t ch, uint8_t pin, uint32_t hz, uint8_t bits) { (void)ch; ledcChangeFrequency(pin, hz, bits); }
inline void wmLedcDetach(uint8_t pin) { ledcDetach(pin); }
#else
inline void wmLedcAttach(uint8_t ch, uint8_t pin, uint32_t hz, uint8_t bits) { ledcSetup(ch, hz, bits); ledcAttachPin(pin, ch); }
inline void wmLedcWrite(uint8_t ch, uint32_t duty) { ledcWrite(ch, duty); }
inline void wmLedcFrequency(uint8_t ch, uint8_t pin, uint32_t hz, uint8_t bits) { (void)pin; ledcChangeFrequency(ch, hz, bits); }
inline void wmLedcDetach(uint8_t pin) { ledcDetachPin(pin); }
#endif
