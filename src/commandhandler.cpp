#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include <CmdParser.hpp>

#include <solarchargershield.h>
#include <internaltemperature.h>

extern SolarChargerShield solarChargerShield;

extern SemaphoreHandle_t solarChargerShieldMutex;

extern InternalTemperature internalTemperature;

extern SemaphoreHandle_t internalTemperatureMutex;

extern TaskHandle_t taskLedHandler;

extern TaskHandle_t taskSolarChargerShieldHandler;

extern TaskHandle_t taskInternalTemperatureHandler;

void pingCommand(CmdParser *myParser)
{
  Serial.println("OK");
}

void watermarkCommand(CmdParser *myParser)
{
  if (myParser->equalCmdParam(1, "led"))
  {
    Serial.println(uxTaskGetStackHighWaterMark(taskLedHandler));
  }
  else if (myParser->equalCmdParam(1, "command"))
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

void TaskCommandHandler(void *pvParameters)
{
  (void)pvParameters;

  Serial.begin(9600);

  CmdCallback<4> cmdCallback;

  CmdBuffer<32> buffer;
  buffer.setEcho(true);

  CmdParser parser;

  cmdCallback.addCmd("ping", &pingCommand);
  cmdCallback.addCmd("watermark", &watermarkCommand);
  cmdCallback.addCmd("voltage", &voltageCommand);
  cmdCallback.addCmd("temperature", &internalTemperatureCommand);

  for (;;)
  {
    vTaskDelay(250 / portTICK_PERIOD_MS); //Delay
    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);
  }
}