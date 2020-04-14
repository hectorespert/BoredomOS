#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include <CmdParser.hpp>

#include <solarchargershield.h>

extern SolarChargerShield solarChargerShield;

extern SemaphoreHandle_t solarChargerShieldMutex;

void pingCommand(CmdParser *myParser)
{
  Serial.println("OK");
}

void voltageCommand(CmdParser *myParser)
{
  if (xSemaphoreTake(solarChargerShieldMutex, 10) == pdTRUE)
  {
    Serial.println(solarChargerShield.voltage);
    xSemaphoreGive(solarChargerShieldMutex);
  } else {
    Serial.println("ERROR");
  }
}

void TaskCommandHandler(void *pvParameters)
{
  (void)pvParameters;

  Serial.begin(9600);

  CmdCallback<3> cmdCallback;

  CmdBuffer<32> buffer;
  buffer.setEcho(true);

  CmdParser parser;

  cmdCallback.addCmd("ping", &pingCommand);
  cmdCallback.addCmd("voltage", &voltageCommand);

  for (;;)
  {
    vTaskDelay(250 / portTICK_PERIOD_MS); //Delay
    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);
  }
}