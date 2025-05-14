#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Log.h>
#include <SD.h>

extern QueueHandle_t loggerQueue;

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    //File logFile = SD.open("log.csv", FILE_WRITE);

    for (;;)
    {
        Log log;
        if (xQueueReceive(loggerQueue, &log, portMAX_DELAY) == pdPASS) {

            Serial.println("Writing to SD card...");
            Serial.println(log.timestamp);
            Serial.println(log.unixtime);
            Serial.println(log.sensors.voltage);
        }
    }

}
