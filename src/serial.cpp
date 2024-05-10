#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

[[noreturn]] void TaskSerial(void *pvParameters)
{
    (void) pvParameters;

    Serial.begin(9600);

    for (;;)
    {
        Serial.println(uxTaskGetStackHighWaterMark(NULL));

        vTaskDelay( 5000 / portTICK_PERIOD_MS );
    }

}