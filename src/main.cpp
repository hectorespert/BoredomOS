#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>

#include <solarchargershield.h>
#include <internaltemperature.h>

#include <RTClib.h>

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

TaskHandle_t taskCommandsHandler;

TaskHandle_t taskInternalTemperatureHandler;

/**
 * Semaphore
 */

SemaphoreHandle_t solarChargerShieldMutex;

SemaphoreHandle_t internalTemperatureMutex;

SemaphoreHandle_t rtcMutex;

/**
 * Global data
 */

SolarChargerShield solarChargerShield;

InternalTemperature internalTemperature;

RTC_DS3231 rtc;

void setup()
{

   Serial.begin(9600);

   /**
    * DS3231 init
    */
   configASSERT(rtc.begin());

   /**
    * RTC configuration if lost power
    */
   if (rtc.lostPower())
   {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
   }

   /**
    * Create mutex
    */
   solarChargerShieldMutex = xSemaphoreCreateMutex();

   internalTemperatureMutex = xSemaphoreCreateMutex();

   rtcMutex = xSemaphoreCreateMutex();

   /**
    *  Create tasks
    */

   xTaskCreate(TaskSolarChargerShield, "SolarCharger", 256, NULL, PRIORITY_HIGH, &taskSolarChargerShieldHandler);

   xTaskCreate(TaskInternalTemperature, "InternalTemperature", 256, NULL, PRIORITY_HIGH, &taskInternalTemperatureHandler);

   xTaskCreate(TaskCommandHandler, "Command", 128, NULL, PRIORITY_LOW, &taskCommandsHandler);
}

void loop()
{
}

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
}
