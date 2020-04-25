#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <solarchargershield.h>
#include <internaltemperature.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3

/**
 * Tasks
 */

extern void TaskLed(void *pvParameters);

extern void TaskCommandHandler(void *pvParameters);

extern void TaskSolarChargerShield(void *pvParameters);

extern void TaskInternalTemperature(void *pvParameters);

/**
   Task handlers
*/
TaskHandle_t taskSolarChargerShieldHandler;

TaskHandle_t taskLedHandler;

TaskHandle_t taskCommandsHandler;

TaskHandle_t taskInternalTemperatureHandler;

/**
 * Semaphore
 */

SemaphoreHandle_t solarChargerShieldMutex;

SemaphoreHandle_t internalTemperatureMutex;

/**
 * Global data
 */

SolarChargerShield solarChargerShield;

InternalTemperature internalTemperature;

void setup()
{

   /**
    * Create mutex
    */
   solarChargerShieldMutex = xSemaphoreCreateMutex();

   internalTemperatureMutex = xSemaphoreCreateMutex();

   /**
    *  Create tasks
    */

   //xTaskCreate(TaskLed, "Led", 128, NULL, PRIORITY_HIGH, &taskLedHandler);

   xTaskCreate(TaskSolarChargerShield, "SolarCharger", 256, NULL, PRIORITY_HIGH, &taskSolarChargerShieldHandler);

   xTaskCreate(TaskInternalTemperature, "InternalTemperature", 256, NULL, PRIORITY_HIGH, &taskInternalTemperatureHandler);

   xTaskCreate(TaskCommandHandler, "Command", 256, NULL, PRIORITY_LOW, &taskCommandsHandler);
}

void loop()
{
}

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
}
