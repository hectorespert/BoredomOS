#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include <CmdParser.hpp>

extern void pingCommand(CmdParser *myParser);

extern void watermarkCommand(CmdParser *myParser);

extern void voltageCommand(CmdParser *myParser);

extern void internalTemperatureCommand(CmdParser *myParser);

void TaskCommandHandler(void *pvParameters)
{
  (void)pvParameters;

  Serial.begin(9600);

  CmdBuffer<32> buffer;
  buffer.setEcho(true);

  CmdParser parser;

  CmdCallback_P<4> cmdCallback;

  cmdCallback.addCmd(PSTR("ping"), &pingCommand);
  cmdCallback.addCmd(PSTR("watermark"), &watermarkCommand);
  cmdCallback.addCmd(PSTR("voltage"), &voltageCommand);
  cmdCallback.addCmd(PSTR("temperature"), &internalTemperatureCommand);

  for (;;)
  {
    vTaskDelay(250 / portTICK_PERIOD_MS); //Delay
    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);
  }
}