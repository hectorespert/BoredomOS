#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SolarCharger.h>

[[noreturn]] void TaskSerial(void *pvParameters)
{
    (void) pvParameters;

    Serial.begin(9600);

    SolarCharger solarCharger(A0);

    for (;;)
    {
        Serial.println(solarCharger.readVoltage());

        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }

}