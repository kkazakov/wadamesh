// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the battery OCV curve, src/helpers/BatteryOcv.h: the
// piecewise-linear voltage->% interpolation that replaces the linear map on
// the L2 and T-Deck ("drops fast, then sits for much longer").
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_battery_ocv.cpp -o /tmp/test_battery_ocv && /tmp/test_battery_ocv
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "helpers/BatteryOcv.h"

// The L2's measured pack curve (Meshtastic seeed_wio_tracker_L2 OCV_ARRAY).
static const uint16_t k_l2[BATTERY_OCV_POINTS] = {
  4180, 4040, 3864, 3800, 3745, 3710, 3687, 3663, 3623, 3482, 3300
};
// The T-Deck's generic LiPo curve.
static const uint16_t k_tdeck[BATTERY_OCV_POINTS] = {
  4200, 4110, 4020, 3950, 3870, 3840, 3800, 3770, 3730, 3690, 3300
};

static int pctL2(uint16_t mv, uint16_t full = 0)    { return batteryOcvPercent(mv, k_l2, BATTERY_OCV_POINTS, full); }
static int pctTDeck(uint16_t mv, uint16_t full = 0) { return batteryOcvPercent(mv, k_tdeck, BATTERY_OCV_POINTS, full); }

int main() {
  // Every table point lands on its exact 10% step, on both curves.
  for (int i = 0; i < BATTERY_OCV_POINTS; ++i) {
    assert(pctL2(k_l2[i]) == 100 - i * 10);
    assert(pctTDeck(k_tdeck[i]) == 100 - i * 10);
  }

  // Clamps: above full is 100 (charger-driven rail), at/below empty is 0.
  assert(pctL2(4300) == 100);
  assert(pctL2(4181) == 100);
  assert(pctL2(3300) == 0);
  assert(pctL2(3000) == 0);
  assert(pctTDeck(4250) == 100);
  assert(pctTDeck(3299) == 0);

  // Midpoints interpolate to the half-step (rounded).
  assert(pctL2((4180 + 4040) / 2) == 95);
  assert(pctL2((3864 + 3800) / 2) == 75);
  assert(pctTDeck((4200 + 4110) / 2) == 95);

  // The regression that started this: the linear map read the L2's 80% point
  // (3864 mV) as 62% and its 50% point (3710 mV) as 45%. The curve must not.
  assert(pctL2(3864) == 80);
  assert(pctL2(3710) == 50);
  assert(pctL2(3623) == 20);

  // Monotonic across the whole range, 1 mV steps: no dips, no jumps backward.
  int prev = 0;
  for (uint32_t mv = 3200; mv <= 4300; ++mv) {
    const int p = pctL2((uint16_t)mv);
    assert(p >= prev && p <= 100);
    prev = p;
  }
  prev = 0;
  for (uint32_t mv = 3200; mv <= 4300; ++mv) {
    const int p = pctTDeck((uint16_t)mv);
    assert(p >= prev && p <= 100);
    prev = p;
  }

  // Calibration scales the whole curve: full = 4100 reads 100 at 4100, and
  // every scaled table point still lands on its step.
  for (int i = 0; i < BATTERY_OCV_POINTS; ++i) {
    const uint16_t scaled = (uint16_t)((uint32_t)k_l2[i] * 4100 / k_l2[0]);
    assert(pctL2(scaled, 4100) == 100 - i * 10);
  }
  assert(pctL2(4100, 4100) == 100);
  assert(pctL2(4180, 4100) == 100);          // above the calibrated full
  // The empty point scales with the calibration too: 3300*4100/4180 = 3236.
  assert(pctL2(3236, 4100) == 0);
  assert(pctL2(3300, 4100) > 0 && pctL2(3300, 4100) < 10);
  // full = 0 means uncalibrated: the table's own top is 100%.
  assert(pctL2(4180, 0) == 100);

  // A 21-point table (5% steps) interpolates too.
  static const uint16_t k21[21] = {
    4200, 4150, 4110, 4080, 4020, 3980, 3950, 3910, 3870, 3850, 3840,
    3820, 3800, 3790, 3770, 3750, 3730, 3710, 3690, 3610, 3300
  };
  for (int i = 0; i < 21; ++i) assert(batteryOcvPercent(k21[i], k21, 21, 0) == 100 - i * 5);
  assert(batteryOcvPercent((4200 + 4150) / 2, k21, 21, 0) == 98);   // 97.5 rounds to 98

  // Degenerate inputs are refused, not divided by zero.
  assert(batteryOcvPercent(3800, nullptr, BATTERY_OCV_POINTS, 0) == -1);
  assert(batteryOcvPercent(3800, k_l2, 1, 0) == -1);
  static const uint16_t k_zero[2] = { 0, 0 };
  assert(batteryOcvPercent(3800, k_zero, 2, 0) == -1);

  printf("test_battery_ocv: all assertions passed\n");
  return 0;
}
