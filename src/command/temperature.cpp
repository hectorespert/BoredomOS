#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <CmdParser.hpp>

#include <internaltemperature.h>

extern InternalTemperature internalTemperature;

extern SemaphoreHandle_t internalTemperatureMutex;

void internalTemperatureCommand(CmdParser *myParser)
{
  if (xSemaphoreTake(internalTemperatureMutex, 10) == pdTRUE)
  {
    Serial.println(internalTemperature.celsius);
    xSemaphoreGive(internalTemperatureMutex);
  }
  else
  {
    Serial.println("ERROR");
  }
}