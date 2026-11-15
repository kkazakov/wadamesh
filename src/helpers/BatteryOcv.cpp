// SPDX-License-Identifier: GPL-3.0-or-later
#include "helpers/BatteryOcv.h"

// Weak default: no measured curve, keep the legacy linear map. A variant with
// one defines a strong boardBatteryOcvTable() in its board driver and the
// linker picks it over this. Both are plain objects in the app image
// (build_src_filter compiles src/ and variants/ directly, no archive), so the
// override always wins — no registration order to get wrong.
__attribute__((weak)) const uint16_t* boardBatteryOcvTable() { return nullptr; }
