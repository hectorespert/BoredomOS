#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <SolarCharger.h>

#include <solarchargershield.h>

extern SolarChargerShield solarChargerShield;

extern SemaphoreHandle_t solarChargerShieldMutex;

void TaskSolarChargerShield(void *pvParameters)
{
  (void) pvParameters;

  SolarCharger solarCharger(A0);

  for (;;)
  {
    float readedVoltage = solarCharger.readVoltage();
    if (xSemaphoreTake(solarChargerShieldMutex, 10) == pdTRUE)
    {
      solarChargerShield.voltage = readedVoltage;
      xSemaphoreGive(solarChargerShieldMutex);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); //Delay
  }
}