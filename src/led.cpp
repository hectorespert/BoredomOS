#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

void TaskLed(void *pvParameters)
{
  (void)pvParameters;

  pinMode(LED_BUILTIN, OUTPUT);

  vTaskDelay(500 / portTICK_PERIOD_MS);
  digitalWrite(LED_BUILTIN, HIGH);
  vTaskDelay(500 / portTICK_PERIOD_MS);
  digitalWrite(LED_BUILTIN, LOW);
  vTaskDelay(500 / portTICK_PERIOD_MS);

  vTaskDelete(NULL);
}