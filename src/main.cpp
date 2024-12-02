#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "Healthcheck.h"
#include "Priority.h"
#include "Energy.h"
#include "Serial.h"

TaskHandle_t taskHealthCheckHandler = NULL;

TaskHandle_t taskEnergyHandler = NULL;

TaskHandle_t taskSerialHandler = NULL;

void setup()
{
    xTaskCreate(TaskHealthCheck, "HealthCheck", 128, NULL, PRIORITY_LOWEST, &taskHealthCheckHandler);

    xTaskCreate(TaskSerial, "Serial", 128, NULL, PRIORITY_LOW, &taskSerialHandler);

    xTaskCreate(TaskEnergy, "Energy", 128, NULL, PRIORITY_LOW, &taskEnergyHandler);
}

void loop()
{

}
