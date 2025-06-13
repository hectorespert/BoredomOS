#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Data.h>
#include <SD.h>
#include <ArduinoJson.h>

extern QueueHandle_t sdWriteQueue;

#define LOG_FILE_COUNT 4
#define LOG_FILE_SIZE_LIMIT (1024UL * 1024UL * 1024UL) // 1GB
#define LOG_INDEX_FILE "data_index.txt"

static int readLogIndex() {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_READ);
    if (!idxFile) return 0;
    int idx = 0;
    idxFile.readBytes((char*)&idx, sizeof(idx));
    idxFile.close();
    if (idx < 0 || idx >= LOG_FILE_COUNT) return 0;
    return idx;
}

static void writeLogIndex(int idx) {
    File idxFile = SD.open(LOG_INDEX_FILE, FILE_WRITE);
    if (idxFile) {
        idxFile.seek(0);
        idxFile.write((uint8_t*)&idx, sizeof(idx));
        idxFile.flush();
        idxFile.close();
    }
}

static String getLogFileName(int idx) {
    return String("data") + idx + ".mpk";
}

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;
    int logFileIdx = readLogIndex();

    for (;;)
    {
        String logFileName = getLogFileName(logFileIdx);
        File dataFile = SD.open(logFileName.c_str(), FILE_WRITE);
        if (!dataFile) {
            return;
        }

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
            serializeMsgPack(doc, dataFile);
            dataFile.flush();
        }

        size_t fileSize = dataFile.size();
        dataFile.close();

        if (fileSize >= LOG_FILE_SIZE_LIMIT) {
            logFileIdx = (logFileIdx + 1) % LOG_FILE_COUNT;
            writeLogIndex(logFileIdx);
            String newLogFileName = getLogFileName(logFileIdx);
            if (SD.exists(newLogFileName.c_str())) {
                SD.remove(newLogFileName.c_str());
            }
        }
    }
}
