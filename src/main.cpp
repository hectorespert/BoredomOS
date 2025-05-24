#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <MAVLink.h>
#include <Sensors.h>
//#include <Wire.h>
#include <RTClib.h>
#include <Serial.h>
#include <Log.h>

RTC_DS1307 rtc;

TaskHandle_t taskSensorsHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

TaskHandle_t taskHeartbeatHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

QueueHandle_t loggerQueue = NULL;

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskDebug(void *pvParameters);

[[noreturn]] extern void TaskMavlink(void *pvParameters);

void setup()
{
  Serial.begin(115200);

  configASSERT(rtc.begin());

  configASSERT(SD.begin(9));

  loggerQueue = xQueueCreate(16, sizeof(Log));
  configASSERT(loggerQueue != NULL);

  serialReadQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialReadQueue != NULL);

  serialWriteQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialWriteQueue != NULL);

  //xTaskCreate(TaskDebug, "TaskDebug", 128, NULL, PRIORITY_HIGHEST, NULL);

  xTaskCreate(TaskSerialRead, "SerialRead", 96, NULL, PRIORITY_HIGHEST, &taskSerialReadHandler);

  xTaskCreate(TaskSerialWrite, "SerialWrite", 128, NULL, PRIORITY_HIGH, &taskSerialWriteHandler);

  xTaskCreate(TaskHeartbeat, "Heartbeat", 96, NULL, PRIORITY_HIGH, &taskHeartbeatHandler);

  xTaskCreate(TaskMavlink, "Mavlink", 256, NULL, PRIORITY_LOW, &taskMavlinkHandler);
  
  xTaskCreate(TaskSensors, "Sensors", 64, NULL, PRIORITY_LOW, &taskSensorsHandler);

  xTaskCreate(TaskLogger, "Logger", 160, NULL, PRIORITY_HIGH, &taskLoggerHandler);

  xTaskCreate(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, &taskSdWriteHandler);

  vTaskStartScheduler();
}

void loop() {}

[[noreturn]] void TaskDebug(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        size_t freeHeap = xPortGetFreeHeapSize();
        Serial.print("Heap libre: ");
        Serial.println(freeHeap);

        Serial.print("TaskDebug water mark ");
        Serial.println(uxTaskGetStackHighWaterMark(NULL));

        if (taskSerialReadHandler != NULL)
        {
            Serial.print("TaskSerialRead water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskSerialReadHandler));
        }

        if (taskSerialWriteHandler != NULL)
        {
            Serial.print("TaskSerialWrite water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskSerialWriteHandler));
        }

        if (taskHeartbeatHandler != NULL)
        {
            Serial.print("TaskHeartbeat water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskHeartbeatHandler));
        }

        if (taskSensorsHandler != NULL)
        {
            Serial.print("TaskSensors water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskSensorsHandler));
        }

        if (taskLoggerHandler != NULL)
        {
            Serial.print("TaskLogger water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskLoggerHandler));
        }

        if (taskSdWriteHandler != NULL)
        {
            Serial.print("TaskSdWrite water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskSdWriteHandler));
        }

        if (taskMavlinkHandler != NULL)
        {
            Serial.print("TaskMavlink water mark ");
            Serial.println(uxTaskGetStackHighWaterMark(taskMavlinkHandler));
        }

        if (loggerQueue != NULL)
        {
            Serial.print("LoggerQueue waiting ");
            Serial.println(uxQueueMessagesWaiting(loggerQueue));
        }

        if (serialReadQueue != NULL)
        {
            Serial.print("SerialReadQueue waiting ");
            Serial.println(uxQueueMessagesWaiting(serialReadQueue));
        }

        if (serialWriteQueue != NULL)
        {
            Serial.print("SerialWriteQueue waiting ");
            Serial.println(uxQueueMessagesWaiting(serialWriteQueue));
        }
        

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
