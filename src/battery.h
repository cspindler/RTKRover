/*******************************************************************************
 * @file battery.h
 * @brief LiPo pack voltage via the Feather's on-board divider.
 *
 * The Huzzah32 ties BAT through a 2:1 resistor divider to A13 (GPIO35), which
 * is ADC1_CH7. GPIO35 is input-only and used by nothing else on this board.
 ******************************************************************************/
#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

/// Configure the ADC channel. Call once from setup(), before the first read.
void batteryInit(void);

/// Pack voltage in millivolts (divider already compensated), averaged over
/// BATTERY_ADC_SAMPLES. Costs ~tens of microseconds; safe from any task.
uint32_t batteryMilliVolts(void);

/// Convenience wrapper for logging.
float getBatteryVolts(void);

#endif /*** BATTERY_H ***/
