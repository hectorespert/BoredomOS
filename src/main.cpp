#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <MAVLink.h>
//#include <Wire.h>
#include <Data.h>
#include <Battery.h>
#include <SystemTime.h>

Battery battery = Battery();

SystemTime systemTime = SystemTime();

TaskHandle_t taskStatusHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

TaskHandle_t taskHeartbeatHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

QueueHandle_t sdWriteQueue = NULL;

[[noreturn]] extern void TaskSerialWrite(void *pvParameters);

[[noreturn]] extern void TaskSerialRead(void *pvParameters);

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskMavlinkBatteryStatus(void *pvParameters);

[[noreturn]] extern void TaskMavlink(void *pvParameters);

void setup()
{
  configASSERT(systemTime.begin());

  Serial.begin(115200);

  configASSERT(SD.begin(9));

  sdWriteQueue = xQueueCreate(16, sizeof(Data*));
  configASSERT(sdWriteQueue != NULL);

  serialReadQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialReadQueue != NULL);

  serialWriteQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialWriteQueue != NULL);

  xTaskCreate(TaskSerialRead, "SerialRead", 96, NULL, PRIORITY_HIGHEST, &taskSerialReadHandler);

  xTaskCreate(TaskSerialWrite, "SerialWrite", 192, NULL, PRIORITY_HIGHEST, &taskSerialWriteHandler);

  xTaskCreate(TaskHeartbeat, "Heartbeat", 128, NULL, PRIORITY_HIGH, &taskHeartbeatHandler);

  xTaskCreate(TaskMavlink, "Mavlink", 256, NULL, PRIORITY_LOW, &taskMavlinkHandler);
  
  xTaskCreate(TaskLogger, "Logger", 96, NULL, PRIORITY_LOW, &taskLoggerHandler);

  xTaskCreate(TaskMavlinkBatteryStatus, "MavlinkBatteryStatus", 128, NULL, PRIORITY_HIGH, &taskStatusHandler);

  xTaskCreate(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, &taskSdWriteHandler);

  vTaskStartScheduler();
}

void loop() {}
