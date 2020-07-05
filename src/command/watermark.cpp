#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <CmdParser.hpp>

extern TaskHandle_t taskLedHandler;

extern TaskHandle_t taskSolarChargerShieldHandler;

extern TaskHandle_t taskInternalTemperatureHandler;

void watermarkCommand(CmdParser *myParser)
{
  if (myParser->equalCmdParam(1, "command"))
  {
    Serial.println(uxTaskGetStackHighWaterMark(NULL));
  }
  else if (myParser->equalCmdParam(1, "charger"))
  {
    Serial.println(uxTaskGetStackHighWaterMark(taskSolarChargerShieldHandler));
  }
  else if (myParser->equalCmdParam(1, "temperature"))
  {
    Serial.println(uxTaskGetStackHighWaterMark(taskInternalTemperatureHandler));
  }
  else
  {
    Serial.println("ERROR");
  }
}