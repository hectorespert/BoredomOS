#include <Arduino.h>
#include <SolarCharger.h>


[[noreturn]] void TaskEnergy(void *pvParameters)
{
    (void) pvParameters;

    SolarCharger solarCharger(A0);

    for (;;)
    {
        float voltage = solarCharger.readVoltage();
    }

}