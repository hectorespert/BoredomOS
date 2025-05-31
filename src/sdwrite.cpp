#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <SD.h>

extern QueueHandle_t sdWriteQueue;

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        Log* log;
        if (xQueueReceive(sdWriteQueue, &log, portMAX_DELAY) == pdPASS) {
            
            String line = String(log->timestamp) + "," + String(log->unixtime) + "," + "0.0" + "\n";
            vPortFree(log);

            File logFile = SD.open("log.csv", FILE_WRITE);
            configASSERT(logFile != NULL);

            if (logFile.size() == 0) {
                logFile.println("timestamp,unixtime,voltage");
                logFile.flush();
            }

            // TODO: Check maximum file size

            logFile.print(line);
            logFile.flush();
            logFile.close();
        }
    }

}
