#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3

/**
  Blink task
*/
void TaskBlink(void *pvParameters)
{
  (void) pvParameters;

  // initialize digital LED_BUILTIN on pin 13 as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  for (;;) // A Task shall never return or exit.
  {
    digitalWrite(LED_BUILTIN, HIGH);   // turn the LED on (HIGH is the voltage level)
    vTaskDelay( 500 / portTICK_PERIOD_MS ); // wait for one second
    digitalWrite(LED_BUILTIN, LOW);    // turn the LED off by making the voltage LOW
    vTaskDelay( 500 / portTICK_PERIOD_MS ); // wait for one second
  }
}

/**
   Tasks
*/
TaskHandle_t taskBlinkHandler;

void setup() {

  /**
     Create tasks
  */
  // Register blink task
  xTaskCreate(TaskBlink, "Blink", 64, NULL, PRIORITY_HIGHEST,  &taskBlinkHandler);

}

void loop() {

}