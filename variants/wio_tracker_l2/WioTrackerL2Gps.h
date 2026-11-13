// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wio Tracker L2 GNSS provider: the core MicroNMEALocationProvider plus the
// expander's GNSS rail switch (TCA9535 bit 13, kGnssPower).
//
// WHY: this board wires no GPIO enable/reset line to the GNSS module — GPS_EN
// and GPS_RESET both resolve to -1 — so the core begin()/stop() are pin no-ops
// and the module keeps drawing full tracking current while the GPS saver
// (Settings > GPS > Save battery, and the battery saver) reports "resting".
// The rail IS switchable, but until now only WioTrackerL2Board::powerOff()
// used it. begin()/stop() are the choke point every GPS on/off path goes
// through (boot initBasicGPS, the Settings toggle, the companion "gps" var,
// the saver's rest/wake), so the rail follows the logical GPS state here —
// no shared-code #ifdef, the quirk stays in the variant.
//
// Trade-off: a rail cut loses the module's ephemeris, so every saver wake is a
// cold start (tens of seconds to minutes under open sky). gpsSaverTick's search
// windows (10 min before the first fix, 3 min after) already cover that. No
// start command is needed after power-up — the module streams NMEA on its own
// — so there is no lost-start failure mode and no boot delay is added here.
//
// UART pins are tri-stated while the rail is off: a driven TX idles HIGH and
// would back-power the unpowered module through its RX ESD diode (the same
// failure the ThinkNode M9 path in WadaNmeaLocationProvider documents).
// NAMING TRAP: Arduino setPins(rx, tx) — PIN_GPS_TX=18 is the pin WE RECEIVE
// ON; PIN_GPS_RX=17 is our TX (confirmed by the bring-up probe, platformio.ini).

#include <Arduino.h>
#include "helpers/sensors/MicroNMEALocationProvider.h"
#include "WioTrackerL2Io.h"

class WioTrackerL2GpsProvider : public MicroNMEALocationProvider {
public:
  WioTrackerL2GpsProvider(HardwareSerial& ser, mesh::RTCClock* clock)
      : MicroNMEALocationProvider(ser, clock), _serial(ser) {}

  void begin() override {
    // Rail first: the module must be powered before its RX sees our TX idle
    // level. Reset is parked released (LOW) by WioTrackerL2Io::begin(), so a
    // restored rail is a clean power-on reset.
    (void)WioTrackerL2Io::setGnssPower(true);
    // Re-attach the UART after stop() tri-stated it. Same pins/order as
    // initBasicGPS() and gpsEnsureBigRxRing(); Serial1 is already begun.
    _serial.setPins(PIN_GPS_TX, PIN_GPS_RX);
    MicroNMEALocationProvider::begin();
    Serial.println("[wio-l2] GNSS rail on");
  }

  void stop() override {
    MicroNMEALocationProvider::stop();
    // Tri-state before the rail goes: never drive TX HIGH into an unpowered
    // module. RX is an input, but park it too (M9 precedent).
    pinMode(PIN_GPS_TX, INPUT);
    pinMode(PIN_GPS_RX, INPUT);
    (void)WioTrackerL2Io::setGnssPower(false);
    Serial.println("[wio-l2] GNSS rail off");
  }

private:
  HardwareSerial& _serial;
};
