// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
struct DateTime {
  uint32_t t = 0;
  DateTime(uint16_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
  // Real RTClib also constructs from a unix timestamp and exposes the fields;
  // the cold-start aiding (UBX-MGA-INI TIME_UTC) uses both. The 6-arg form still
  // leaves t at 0, so unixtime() behaves exactly as it did for existing tests.
  explicit DateTime(uint32_t unixtime) : t(unixtime) {}
  uint32_t unixtime() const { return t; }
  uint16_t year() const { return 2026; }
  uint8_t month() const { return 1; }
  uint8_t day() const { return 1; }
  uint8_t hour() const { return 0; }
  uint8_t minute() const { return 0; }
  uint8_t second() const { return (uint8_t)(t % 60); }
};
