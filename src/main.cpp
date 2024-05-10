#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "healthcheck.h"
#include "priority.h"
#include "serial.h"

TaskHandle_t taskHealthCheckHandler = NULL;

void setup()
{
    xTaskCreate(TaskHealthCheck, "HealthCheck", 128, nullptr, PRIORITY_LOWEST, &taskHealthCheckHandler);

    xTaskCreate(TaskSerial, "Serial", 128, NULL, PRIORITY_LOW, NULL);
}

void loop()
{

}
