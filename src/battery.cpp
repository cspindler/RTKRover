#include "battery.h"

#include <RTKRoverConfig.h>

void batteryInit()
{
  // 11 dB is the arduino-esp32 default, but pin it explicitly: the full-scale
  // input has to cover a charging pack (4.2 V / 2 = 2.1 V at the pin), and a
  // lower attenuation would clip there and silently read flat.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
}

uint32_t batteryMilliVolts()
{
  // analogReadMilliVolts() applies the per-chip eFuse ADC calibration; raw
  // analogRead() * 3.3/4095 is off by 100+ mV on the ESP32's non-linear ADC.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++)
  {
    sum += analogReadMilliVolts(BATTERY_ADC_PIN);
  }
  return (sum / BATTERY_ADC_SAMPLES) * BATTERY_DIVIDER_RATIO;
}

float getBatteryVolts()
{
  return batteryMilliVolts() / 1000.0f;
}
