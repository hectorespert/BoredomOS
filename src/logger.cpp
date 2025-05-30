#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <RTC.h>
#include <Sensors.h>
#include <System.h>

extern QueueHandle_t sdWriteQueue;
extern Sensors sensors;
extern TaskHandle_t taskStatusHandler;
extern TaskHandle_t taskLoggerHandler;
extern TaskHandle_t taskHeartbeatHandler;
extern TaskHandle_t taskSensorsHandler;
extern TaskHandle_t taskSdWriteHandler;
extern TaskHandle_t taskMavlinkHandler;
extern TaskHandle_t taskSerialWriteHandler;
extern TaskHandle_t taskSerialReadHandler;

[[noreturn]] void TaskLogger(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        RTCTime currentTime;
        RTC.getTime(currentTime);

        Log log = {
            timestamp: currentTime.toString(),
            unixtime: currentTime.getUnixTime(),
            uptime: xTaskGetTickCount() * portTICK_PERIOD_MS,
            system: {
                heap: xPortGetFreeHeapSize(),
                taskLoggerAvailableStack: uxTaskGetStackHighWaterMark(taskLoggerHandler),
                taskHeartbeatAvailableStack: uxTaskGetStackHighWaterMark(taskHeartbeatHandler),
                taskSensorsAvailableStack: uxTaskGetStackHighWaterMark(taskSensorsHandler),
                taskStatusAvailableStack: uxTaskGetStackHighWaterMark(taskStatusHandler),
                taskSdWriteAvailableStack: uxTaskGetStackHighWaterMark(taskSdWriteHandler),
                taskMavlinkAvailableStack: uxTaskGetStackHighWaterMark(taskMavlinkHandler),
                taskSerialReadAvailableStack: uxTaskGetStackHighWaterMark(taskSerialReadHandler),
                taskSerialWriteAvailableStack: uxTaskGetStackHighWaterMark(taskSerialWriteHandler),
            },
            sensors: sensors,
        };

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