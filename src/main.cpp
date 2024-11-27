#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "Healthcheck.h"
#include "Priority.h"
#include "Serial.h"

TaskHandle_t taskHealthCheckHandler = NULL;

void setup()
{
    xTaskCreate(TaskHealthCheck, "HealthCheck", 128, NULL, PRIORITY_LOWEST, &taskHealthCheckHandler);

    xTaskCreate(TaskSerial, "Serial", 128, NULL, PRIORITY_LOW, NULL);
}

void loop()
{

}
