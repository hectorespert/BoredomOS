#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3


extern void TaskInitLed(void *pvParameters); 

/**
   Task handlers
*/
TaskHandle_t taskTaskInitLedHandler;

void setup() {

  /**
     Create tasks
  */
  xTaskCreate(TaskInitLed, "InitLed", 64, NULL, PRIORITY_HIGHEST,  &taskTaskInitLedHandler);

}

void loop() {

}
