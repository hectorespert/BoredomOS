#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include <CmdParser.hpp>

#define INIT_COUNT 40 // 10 seconds

CmdCallback<3> cmdCallback;

CmdBuffer<32> buffer;
CmdParser parser;

boolean enabled = false;

void enableCommand(CmdParser *myParser) { 
  Serial.println("Enabling debug console");
  enabled = true;
}

void disableCommand(CmdParser *myParser) {
  Serial.println("Disabling debug console");
  enabled = false;
}

void TaskCommands(void *pvParameters)
{
  (void) pvParameters;

  boolean init = true;

  Serial.begin(9600);

  cmdCallback.addCmd("enable", &enableCommand);
  cmdCallback.addCmd("disable", &disableCommand);

  buffer.setEcho(true);

  Serial.println("BoredomOS debug console");
  Serial.println("Send 'enable' to enable debug console");

  int initCount = 0;
  do {
    vTaskDelay( 250 / portTICK_PERIOD_MS ); // Init delay

    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);

    if (initCount >= INIT_COUNT) {
      init = false;
    }
    initCount++;
  } while (init);

  if (enabled)
  {
    Serial.println("Debug console enabled");
  }

  while (enabled)
  {
    vTaskDelay( 250 / portTICK_PERIOD_MS ); //Delay

    cmdCallback.updateCmdProcessing(&parser, &buffer, &Serial);
    
  }
  


  Serial.println("Debug console disabled");

  vTaskDelete(NULL);
}