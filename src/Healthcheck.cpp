#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define LED_HIGH_MS 250
#define LED_LOW_MS 59750

[[noreturn]] void TaskHealthCheck(void *pvParameters)
{
    (void) pvParameters;

    pinMode(LED_BUILTIN, OUTPUT);

    for (;;)
    {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay(LED_HIGH_MS / portTICK_PERIOD_MS);
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay(LED_LOW_MS / portTICK_PERIOD_MS);
    }

}
