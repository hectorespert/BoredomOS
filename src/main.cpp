#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <solarchargershield.h>

#define PRIORITY_LOWEST 0
#define PRIORITY_LOW 1
#define PRIORITY_HIGH 2
#define PRIORITY_HIGHEST 3

/**
 * Tasks
 */

extern void TaskInitLed(void *pvParameters);

extern void TaskCommandHandler(void *pvParameters);

extern void TaskSolarChargerShield(void *pvParameters);

/**
   Task handlers
*/
TaskHandle_t taskSolarChargerShieldHandler;

TaskHandle_t taskInitLedHandler;

TaskHandle_t taskCommandsHandler;

/**
 * Semaphore
 */

SemaphoreHandle_t solarChargerShieldMutex;

SolarChargerShield solarChargerShield;

void setup()
{

   /**
    * Create mutex
    */
   solarChargerShieldMutex = xSemaphoreCreateMutex();

   /**
    *  Create tasks
    */

   xTaskCreate(TaskInitLed, "InitLed", 64, NULL, PRIORITY_HIGH, &taskInitLedHandler);

   xTaskCreate(TaskSolarChargerShield, "SolarCharger", 258, NULL, PRIORITY_HIGH, &taskSolarChargerShieldHandler);

   xTaskCreate(TaskCommandHandler, "Commands", 258, NULL, PRIORITY_HIGH, &taskCommandsHandler);
}

void loop()
{
}

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
}
