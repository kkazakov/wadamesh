#include <Arduino.h>
#include "TDeckBoard.h"
#include "helpers/BatteryOcv.h"

// Generic LiPo discharge curve, 10% steps (100%..0%): the standard 21-point
// Li-ion table (4.20/4.11/4.02/3.95/3.87/3.84/3.80/3.77/3.73/3.69/3.27 V)
// decimated to 11, with the empty point at the firmware's 3.30 V cutoff. The
// T-Deck has no fuel gauge; the linear map this replaces rushed the top third
// of the discharge and stalled on the 3.7-3.8 V plateau. Overrides the weak
// default in src/helpers/BatteryOcv.cpp.
const uint16_t* boardBatteryOcvTable() {
  static const uint16_t k_ocv[BATTERY_OCV_POINTS] = {
    4200, 4110, 4020, 3950, 3870, 3840, 3800, 3770, 3730, 3690, 3300
  };
  return k_ocv;
}

uint32_t deviceOnline = 0x00;

void TDeckBoard::begin() {
  
  ESP32Board::begin();
  
  // Enable peripheral power
  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);

  // Configure user button
  pinMode(PIN_USER_BTN, INPUT);

  // Configure LoRa Pins
  pinMode(P_LORA_MISO, INPUT_PULLUP);
  // pinMode(P_LORA_DIO_1, INPUT_PULLUP);

  #ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, HIGH); // inverted pin for SX1276 - HIGH for off
  #endif

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET; // received a LoRa packet (while in deep sleep)
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}