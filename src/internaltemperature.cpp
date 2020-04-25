#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <PreciseLM35.h>

#include <internaltemperature.h>

extern InternalTemperature internalTemperature;

extern SemaphoreHandle_t internalTemperatureMutex;

void TaskInternalTemperature(void *pvParameters)
{
  (void) pvParameters;

  PreciseLM35 lm35(A1, DEFAULT);

  for (;;)
  {
    if (xSemaphoreTake(internalTemperatureMutex, 10) == pdTRUE)
    {
      internalTemperature.celsius = lm35.readCelsius();
      xSemaphoreGive(internalTemperatureMutex);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); //Delay
  }

}