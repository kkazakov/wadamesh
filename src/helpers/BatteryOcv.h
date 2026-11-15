// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

// Li-ion state of charge from terminal voltage, interpolated over a measured
// open-circuit-voltage (OCV) table. A linear mV->% map cannot work: the cell
// drops steeply from full, then spends most of its capacity on a flat plateau
// around 3.7-3.8 V, so a linear map rushes the first third of the discharge
// and stalls in the middle — the "drops fast, then sits for much longer"
// report on the L2 Pro and T-Deck.
//
// Table contract: n descending mV points at EQUAL state-of-charge steps,
// ocv[0] = 100% ... ocv[n-1] = 0%, with (n-1) dividing 100 (11 points = 10%
// steps, 21 = 5%). full_mv is the voltage that must read 100% — the user's
// "Calibrate as full" value, or ocv[0] when uncalibrated (pass 0). The whole
// table is scaled by full_mv/ocv[0] so calibration keeps the curve's shape.
//
// Pure and header-only so the host tests can run it without the firmware.

static constexpr uint8_t BATTERY_OCV_POINTS = 11;   // 10% steps, 100%..0%

static inline int batteryOcvPercent(uint16_t mv, const uint16_t* ocv, uint8_t n, uint16_t full_mv) {
  if (!ocv || n < 2 || ocv[0] == 0) return -1;
  if (full_mv == 0) full_mv = ocv[0];
  if (mv >= full_mv) return 100;
  const uint32_t bottom = (uint32_t)ocv[n - 1] * full_mv / ocv[0];
  if (mv <= bottom) return 0;
  const uint32_t step = 100u / (n - 1);            // % per table segment
  for (uint8_t i = 1; i < n; ++i) {
    const uint32_t lo = (uint32_t)ocv[i] * full_mv / ocv[0];
    if (mv < lo) continue;
    const uint32_t hi   = (uint32_t)ocv[i - 1] * full_mv / ocv[0];
    const uint32_t base = (uint32_t)(n - 1 - i) * step;
    const uint32_t span = hi - lo;
    const uint32_t frac = span ? ((uint32_t)(mv - lo) * step + span / 2) / span : 0;
    const uint32_t pct  = base + frac;
    return (int)(pct > 100 ? 100 : pct);
  }
  return 0;
}

// Per-board OCV table, or nullptr to keep the legacy linear map. The base
// board class lives in the pinned upstream core (lib_deps), so it cannot take
// a virtual hook; instead a variant with a measured curve overrides this weak
// default at link time (see BatteryOcv.cpp and the board drivers). Shared code
// stays board-agnostic — no #ifdef — and boards without a table keep their
// existing behavior until one is measured for them.
const uint16_t* boardBatteryOcvTable();
