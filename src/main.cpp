#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3

/**
 * Task functions
 */
extern void TaskInitLed(void *pvParameters);

extern void TaskCommands(void *pvParameters); 

/**
   Task handlers
*/
TaskHandle_t taskInitLedHandler;

TaskHandle_t taskCommandsHandler;

void setup() {

  /**
     Create tasks
  */
  xTaskCreate(TaskInitLed, "InitLed", 64, NULL, PRIORITY_HIGHEST,  &taskInitLedHandler);

  xTaskCreate(TaskCommands, "Commands", 128, NULL, PRIORITY_LOWEST,  &taskCommandsHandler);

}

void loop() {

}
