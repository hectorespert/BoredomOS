#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <SD.h>

extern QueueHandle_t loggerQueue;

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        Log log;
        if (xQueueReceive(loggerQueue, &log, portMAX_DELAY) == pdPASS) {
            
            String line = String(log.timestamp) + "," + String(log.unixtime) + "," + String(log.sensors.voltage, 3) + "\n";

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
