#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <RTClib.h>
#include <Sensors.h>

extern RTC_DS1307 rtc;
extern QueueHandle_t loggerQueue;
extern Sensors sensors;

[[noreturn]] void TaskLogger(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        DateTime now = rtc.now();

        Log log = {
            timestamp: now.timestamp(),
            unixtime: now.unixtime(),
            sensors: sensors,
        };

        xQueueSend(loggerQueue, &log, portMAX_DELAY);
        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }


}