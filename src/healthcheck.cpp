#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define LED_HIGH_MS 250 / portTICK_PERIOD_MS
#define LED_LOW_MS (60000 - LED_HIGH_MS) / portTICK_PERIOD_MS

[[noreturn]] void TaskHealthCheck(void *pvParameters)
{
    (void) pvParameters;

    pinMode(LED_BUILTIN, OUTPUT);

    for (;;)
    {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay(LED_HIGH_MS);
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay(LED_LOW_MS);
    }

}
