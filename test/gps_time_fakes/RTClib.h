// SPDX-License-Identifier: GPL-3.0-or-later
// RTClib's DateTime, as far as the provider uses it: a calendar date and time -> Unix seconds
// (UTC), the same days-from-civil arithmetic RTClib does for 2000..2099. The reverse
// (Unix -> calendar) is what the cold-start aiding (UBX-MGA-INI TIME_UTC) reads back.
#pragma once
#include <stdint.h>
struct DateTime {
  uint32_t _t;
  uint16_t _y; uint8_t _mo, _d, _h, _mi, _s;
  DateTime(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s)
      : _y(y), _mo(mo), _d(d), _h(h), _mi(mi), _s(s) {
    const int yy = (int)y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153u * (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long days = (long)era * 146097L + (long)doe - 719468L;
    _t = (uint32_t)(days * 86400L + h * 3600L + mi * 60L + s);
  }
  // Real RTClib also constructs from a Unix timestamp and exposes the fields; the
  // cold-start aiding reads both. civil-from-days, the inverse of the ctor above.
  explicit DateTime(uint32_t t) : _t(t) {
    const long z = (long)(t / 86400u) + 719468L;
    const long era = (z >= 0 ? z : z - 146096L) / 146097L;
    const unsigned doe = (unsigned)(z - era * 146097L);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    _d = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    _mo = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    _y = (uint16_t)(yoe + era * 400 + (_mo <= 2));
    const uint32_t sod = t % 86400u;
    _h = (uint8_t)(sod / 3600); _mi = (uint8_t)(sod / 60 % 60); _s = (uint8_t)(sod % 60);
  }
  uint32_t unixtime() const { return _t; }
  uint16_t year() const { return _y; }
  uint8_t month() const { return _mo; }
  uint8_t day() const { return _d; }
  uint8_t hour() const { return _h; }
  uint8_t minute() const { return _mi; }
  uint8_t second() const { return _s; }
};
