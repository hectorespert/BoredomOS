#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <RTClib.h>
#include <Sensors.h>
#include <System.h>

extern RTC_DS1307 rtc;
extern QueueHandle_t sdWriteQueue;
extern QueueHandle_t mavlinkStatusQueue;
extern Sensors sensors;
extern TaskHandle_t taskStatusHandler;
extern TaskHandle_t taskLoggerHandler;
extern TaskHandle_t taskHeartbeatHandler;
extern TaskHandle_t taskSensorsHandler;
extern TaskHandle_t taskSdWriteHandler;

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
            system: {
                heap: xPortGetFreeHeapSize(),
                taskLoggerAvailableStack: uxTaskGetStackHighWaterMark(taskLoggerHandler),
                taskHeartbeatAvailableStack: uxTaskGetStackHighWaterMark(taskHeartbeatHandler),
                taskSensorsAvailableStack: uxTaskGetStackHighWaterMark(taskSensorsHandler),
                taskStatusAvailableStack: uxTaskGetStackHighWaterMark(taskStatusHandler),
                taskSdWriteAvailableStack: uxTaskGetStackHighWaterMark(taskSdWriteHandler),
            },
            sensors: sensors,
        };

        Log* logToMavlinkStatus = (Log*) pvPortMalloc(sizeof(Log));
        if (logToMavlinkStatus != NULL) {
            memcpy(logToMavlinkStatus, &log, sizeof(Log));
            if (xQueueSend(mavlinkStatusQueue, &logToMavlinkStatus, 0) != pdPASS) {
                vPortFree(logToMavlinkStatus);
            }
        }

        Log* logToSdCard = (Log*) pvPortMalloc(sizeof(Log));
        if (logToSdCard != NULL) {
            memcpy(logToSdCard, &log, sizeof(Log));
            if (xQueueSend(sdWriteQueue, &logToSdCard, 0) != pdPASS) {
                vPortFree(logToSdCard);
            }
        }

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }


}