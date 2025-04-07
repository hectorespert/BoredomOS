#include <Sensors.h>
#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SolarCharger.h>

SolarCharger solarCharger(A0);

Sensors sensors = {0.0f};

[[noreturn]] void TaskSensors(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        sensors.voltage = solarCharger.readVoltage();
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }
}
