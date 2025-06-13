#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Data.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <SdData.h>

extern QueueHandle_t sdWriteQueue;

SdData sdData;

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    sdData.begin();

    for (;;)
    {
        Data* data;
        if (xQueueReceive(sdWriteQueue, &data, portMAX_DELAY) == pdPASS) {
            JsonDocument doc;
            doc["unixtime"] = data->unixtime;
            doc["uptime"] = data->uptime;
            doc["system"]["heap"] = data->system.heap;
            doc["system"]["tasks"]["loggerAvailableStack"] = data->system.tasks.loggerAvailableStack;
            doc["system"]["tasks"]["heartbeatAvailableStack"] = data->system.tasks.heartbeatAvailableStack;
            doc["system"]["tasks"]["statusAvailableStack"] = data->system.tasks.statusAvailableStack;
            doc["system"]["tasks"]["sdWriteAvailableStack"] = data->system.tasks.sdWriteAvailableStack;
            doc["system"]["tasks"]["mavlinkAvailableStack"] = data->system.tasks.mavlinkAvailableStack;
            doc["system"]["tasks"]["serialReadAvailableStack"] = data->system.tasks.serialReadAvailableStack;
            doc["system"]["tasks"]["serialWriteAvailableStack"] = data->system.tasks.serialWriteAvailableStack;
            doc["energy"]["millivolts"] = data->energy.millivolts;
            doc["energy"]["remaining"] = data->energy.remaining;
            vPortFree(data);
            sdData.write(doc);
        }
    }
}
