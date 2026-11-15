// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

CrowPanel35Board board;
static SPIClass radioSpi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, radioSpi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
// No GPS_EN/GPS_RESET line on this board, so the core provider's stop() is a pin
// no-op and the receiver kept drawing full tracking current through every rest of
// the GPS saver. The Wada provider sends UBX-CFG-RST (controlled GNSS stop/start)
// instead and retries the start until sentences flow again. A receiver that does
// not speak UBX ignores the binary message, so a non-u-blox module behaves as it
// did before. Same UART and baud handling as the core provider -- initBasicGPS()
// still owns Serial1; no baud probe is opted into here.
WadaNmeaLocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);
#else
EnvironmentSensorManager sensors;
#endif

CrowPanel35Display display;

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  if (!radio.std_init(&radioSpi)) {
    Serial.println("[crowpanel-35] SX1262 not found on the LoRa expansion slot");
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
