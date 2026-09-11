/*******************************************************************************
 * @file led.h
 * @brief The board's red LED: boot and runtime blink codes (README.md).
 ******************************************************************************/
#ifndef LED_H
#define LED_H

#include <Arduino.h>

void ledInit();

/**
 * @brief One on/off blink of blinkTime ms each.
 *
 * @param blinkTime   On time and off time, ms
 * @param doNotBlock  true: vTaskDelay (yields to other tasks); false: delay()
 */
void blinkOneTime(int blinkTime, bool doNotBlock);

#endif /*** LED_H ***/
