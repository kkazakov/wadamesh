// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

TDeckProBoard board;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);
WadaNmeaLocationProvider gps(Serial1, &rtc_clock, GPS_RESET, GPS_EN,
                             PIN_GPS_TX, PIN_GPS_RX, GPS_BAUD_RATE);
EnvironmentSensorManager sensors(gps);
TDeckProDisplay display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);

SPIClass* tdeckSharedSPI() { return &SPI; }

// Cold-start aiding: this board cuts the GNSS rail through GPS_EN on every stop,
// so each saver wake loses the ephemeris and searches the whole sky. Hand the
// provider the node's last position and whether the clock can be trusted; it sends
// UBX-MGA-INI time/position ~3 s after begin(), once the MIA-M10Q is listening.
static bool tdeckProGpsAidPos(double* lat, double* lon) {
  *lat = sensors.node_lat;
  *lon = sensors.node_lon;
  return true;
}

bool radio_init() {
  fallback_clock.begin();
  gps.setAidSource(tdeckProGpsAidPos, [] { return rtc_clock.timeIsCurrent(); });
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}