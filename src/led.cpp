//
// Created by hector on 23/6/21.
//

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

void TaskLed(void *pvParameters)
{
    (void) pvParameters;

    pinMode(LED_BUILTIN, OUTPUT);

    for (;;)
    {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay( 250 / portTICK_PERIOD_MS );
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay( 1750 / portTICK_PERIOD_MS );
    }

    vTaskDelete(NULL);
}
