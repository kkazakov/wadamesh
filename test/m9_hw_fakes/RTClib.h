// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
struct DateTime {
  DateTime(uint16_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
  // Real RTClib also constructs from a Unix timestamp and exposes the fields; the
  // cold-start aiding (UBX-MGA-INI TIME_UTC) reads both. The M9 board test does not
  // exercise aiding, so stub values are enough to compile the shared provider.
  explicit DateTime(uint32_t) {}
  uint32_t unixtime() const { return 0; }
  uint16_t year() const { return 2026; }
  uint8_t month() const { return 1; }
  uint8_t day() const { return 1; }
  uint8_t hour() const { return 0; }
  uint8_t minute() const { return 0; }
  uint8_t second() const { return 0; }
};
