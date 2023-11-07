#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

[[noreturn]] void TaskHealthCheck(void *pvParameters)
{
    (void) pvParameters;

    pinMode(LED_BUILTIN, OUTPUT);

    for (;;)
    {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay( 250 / portTICK_PERIOD_MS );
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay( 4750 / portTICK_PERIOD_MS );
    }

}
