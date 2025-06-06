#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Data.h>
#include <SystemTime.h>

extern SystemTime systemTime;

extern QueueHandle_t sdWriteQueue;

extern TaskHandle_t taskStatusHandler;
extern TaskHandle_t taskLoggerHandler;
extern TaskHandle_t taskHeartbeatHandler;
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
        Data data = {
            unixtime: systemTime.getUnixTime(),
            uptime: xTaskGetTickCount() * portTICK_PERIOD_MS,
            system: {
                heap: xPortGetFreeHeapSize(),
                tasks: {
                    loggerAvailableStack: uxTaskGetStackHighWaterMark(taskLoggerHandler),
                    heartbeatAvailableStack: uxTaskGetStackHighWaterMark(taskHeartbeatHandler),
                    statusAvailableStack: uxTaskGetStackHighWaterMark(taskStatusHandler),
                    sdWriteAvailableStack: uxTaskGetStackHighWaterMark(taskSdWriteHandler),
                    mavlinkAvailableStack: uxTaskGetStackHighWaterMark(taskMavlinkHandler),
                    serialReadAvailableStack: uxTaskGetStackHighWaterMark(taskSerialReadHandler),
                    serialWriteAvailableStack: uxTaskGetStackHighWaterMark(taskSerialWriteHandler),
                }
            }
        };

        Data* dataToSdCard = (Data*) pvPortMalloc(sizeof(Data));
        if (dataToSdCard != NULL) {
            memcpy(dataToSdCard, &data, sizeof(Data));
            if (xQueueSend(sdWriteQueue, &dataToSdCard, 0) != pdPASS) {
                vPortFree(dataToSdCard);
            }
        }

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }

}
