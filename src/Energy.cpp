#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SolarCharger.h>

extern TaskHandle_t taskSerialHandler;

[[noreturn]] void TaskEnergy(void *pvParameters)
{
    (void) pvParameters;

    SolarCharger solarCharger(A0);

    for (;;)
    {
        float voltage = solarCharger.readVoltage();

        xTaskNotify(taskSerialHandler, *(uint32_t*)&voltage, eSetValueWithOverwrite);

        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }

}