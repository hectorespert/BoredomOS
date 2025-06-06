#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Data.h>
#include <SD.h>

extern QueueHandle_t sdWriteQueue;

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        Data* data;
        if (xQueueReceive(sdWriteQueue, &data, portMAX_DELAY) == pdPASS) {
            
            String line = String(data->unixtime) + "," + "0.0" + "\n";
            vPortFree(data);

            File logFile = SD.open("log.csv", FILE_WRITE);
            configASSERT(logFile != NULL);

            if (logFile.size() == 0) {
                logFile.println("unixtime,voltage");
                logFile.flush();
            }

            // TODO: Check maximum file size

            logFile.print(line);
            logFile.flush();
            logFile.close();
        }
    }

}
