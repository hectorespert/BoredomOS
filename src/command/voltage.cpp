#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <CmdParser.hpp>

#include <solarchargershield.h>

extern SolarChargerShield solarChargerShield;

extern SemaphoreHandle_t solarChargerShieldMutex;

void voltageCommand(CmdParser *myParser)
{
  if (xSemaphoreTake(solarChargerShieldMutex, 10) == pdTRUE)
  {
    Serial.println(solarChargerShield.voltage);
    xSemaphoreGive(solarChargerShieldMutex);
  }
  else
  {
    Serial.println("ERROR");
  }
}