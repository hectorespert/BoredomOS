#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include <CmdParser.hpp>

void pingCommand(CmdParser *myParser) {
  Serial.println("OK");
}

void TaskCommands(void *pvParameters)
{
  (void) pvParameters;

  Serial.begin(9600);

  CmdCallback<3> cmdCallback;

  CmdBuffer<32> buffer;
  buffer.setEcho(true);
  
  CmdParser parser;

  cmdCallback.addCmd("ping", &pingCommand);

  for (;;) {
    vTaskDelay( 250 / portTICK_PERIOD_MS ); //Delay
    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);
  }

}