#include "led.h"

void ledInit()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
}

void blinkOneTime(int blinkTime, bool doNotBlock)
{
  digitalWrite(LED_BUILTIN, HIGH);
  doNotBlock ? vTaskDelay(blinkTime) : delay(blinkTime);
  digitalWrite(LED_BUILTIN, LOW);
  doNotBlock ? vTaskDelay(blinkTime) : delay(blinkTime);
}
