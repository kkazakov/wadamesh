// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wio Tracker L2 GNSS provider: the core MicroNMEALocationProvider plus the
// expander's GNSS rail switch (TCA9535 bit 13, kGnssPower).
//
// WHY: this board wires no GPIO enable/reset line to the GNSS module — GPS_EN
// and GPS_RESET both resolve to -1 — so the core begin()/stop() are pin no-ops
// and the module keeps drawing full tracking current while the GPS saver
// reports "resting". The rail IS switchable, but until now only
// WioTrackerL2Board::powerOff() used it. begin()/stop() are the choke point
// every GPS on/off path goes through (boot initBasicGPS, the Settings toggle,
// the companion "gps" var, the saver's rest/wake) — no shared-code #ifdef, the
// quirk stays in the variant.
//
// GATE: cutting the rail is part of the Battery saver (opt-in), mirroring the
// panel-sleep gate in WioTrackerL2Display. With the saver OFF, stop() leaves
// the module powered exactly as before — GPS off/on stays warm (fast re-fix)
// but draws current; with it ON, stop() cuts the rail so "resting" truly
// powers the module down. This keys on the Battery saver only: GPS > Save
// battery alone still rests the receiver but does NOT cut the rail. begin()
// always powers the rail — GPS cannot start without it.
//
// Trade-off (saver on): a rail cut loses the module's ephemeris, so every
// saver wake is a cold start (tens of seconds to minutes under open sky).
// gpsSaverTick's search windows (10 min before the first fix, 3 min after)
// already cover that. No start command is needed after power-up — the module
// streams NMEA on its own — so there is no lost-start failure mode and no boot
// delay is added here.
//
// UART pins are tri-stated while the rail is off: a driven TX idles HIGH and
// would back-power the unpowered module through its RX ESD diode (the same
// failure the ThinkNode M9 path in WadaNmeaLocationProvider documents).
// NAMING TRAP: three different pin orders meet here. Arduino setPins(rx, tx)
// and IDF uart_set_pin(port, tx, rx, ...) are opposites — PIN_GPS_TX=18 is the
// pin WE RECEIVE ON (ESP RX); PIN_GPS_RX=17 is our TX (ESP TX), confirmed by
// the bring-up probe (platformio.ini).

#include <Arduino.h>
#include <driver/uart.h>   // uart_set_pin: re-route TX after stop() detached it (see begin)
#include "helpers/sensors/MicroNMEALocationProvider.h"
#include "WioTrackerL2Io.h"
#include "TouchSleep.h"   // touchSleep::enabled() -- the Battery saver gates the rail cut

class WioTrackerL2GpsProvider : public MicroNMEALocationProvider {
public:
  WioTrackerL2GpsProvider(HardwareSerial& ser, mesh::RTCClock* clock)
      : MicroNMEALocationProvider(ser, clock), _serial(ser) {}

  void begin() override {
    // Rail first: the module must be powered before its RX sees our TX idle
    // level. Reset is parked released (LOW) by WioTrackerL2Io::begin(), so a
    // restored rail is a clean power-on reset.
    (void)WioTrackerL2Io::setGnssPower(true);
    // Re-attach the UART after stop() tri-stated it. NOT HardwareSerial::setPins:
    // on arduino-esp32 2.0.17 uartSetPins() only (re)attaches a pin that DIFFERS
    // from the one it recorded, and these pins never change — so setPins() here
    // is a no-op. But stop()'s pinMode(PIN_GPS_RX, INPUT) re-selects the GPIO17
    // pad's IOMUX function to plain GPIO (MCU_SEL 2=U1TXD -> 1=GPIO; GPIO17 is
    // U1TXD's native IOMUX pin, so it routes via the pad, not the GPIO matrix),
    // disconnecting our TX. Without a real re-route TX stays a floating input for
    // the rest of the session (RX keeps working, so NMEA still arrives and the gap
    // hides). uart_set_pin() re-selects the pad back to U1TXD and restores a driven,
    // idle-HIGH output. Verified on the board: pad MCU_SEL 2->1 on stop, 1->2 on
    // begin. Serial1 is UART_NUM_1 here (HardwareSerial(1)); IDF order is (tx, rx):
    // PIN_GPS_RX=17 is our TX, PIN_GPS_TX=18 our RX.
    uart_set_pin(UART_NUM_1, PIN_GPS_RX, PIN_GPS_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    MicroNMEALocationProvider::begin();
    Serial.println("[wio-l2] GNSS rail on");
  }

  void stop() override {
    MicroNMEALocationProvider::stop();
    // Rail cutting is part of the Battery saver (opt-in): with the saver off
    // the module stays powered when GPS stops, exactly as before (warm, fast
    // re-enable, but drawing current). With it on, cut the rail so "resting"
    // truly powers the module down.
    if (!touchSleep::enabled()) return;
    // Tri-state before the rail goes: never drive TX HIGH into an unpowered
    // module. RX is an input, but park it too (M9 precedent). Note pinMode(INPUT)
    // on our TX (GPIO17) also re-selects the pad to plain GPIO, detaching U1TXD —
    // begin() must re-route it with uart_set_pin(), not setPins() (see there).
    pinMode(PIN_GPS_TX, INPUT);
    pinMode(PIN_GPS_RX, INPUT);
    (void)WioTrackerL2Io::setGnssPower(false);
    Serial.println("[wio-l2] GNSS rail off");
  }

private:
  HardwareSerial& _serial;
};
