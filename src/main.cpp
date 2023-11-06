#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3

/**
 * Tasks
 */

[[noreturn]] extern void TaskLed(void *pvParameters);


void setup()
{
    xTaskCreate(TaskLed, "Led", 128, nullptr, PRIORITY_HIGHEST, nullptr);
}

void loop()
{
}
