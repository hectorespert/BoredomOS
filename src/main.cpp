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

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

QueueHandle_t loggerQueue = NULL;

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

void setup()
{
  Serial.begin(115200);

  configASSERT(rtc.begin());

  configASSERT(SD.begin(9));

  loggerQueue = xQueueCreate(16, sizeof(Log));
  configASSERT(loggerQueue != NULL);


  /*

  logFile = SD.open("datalog.txt", FILE_WRITE);
  if (!logFile) {
    Serial.print("error opening ");
    while (true);
  }*/

  //serialReadQueue = xQueueCreate(16, sizeof(mavlink_message_t));

  //serialWriteQueue = xQueueCreate(16, sizeof(mavlink_message_t));

  //xTaskCreate(TaskSerialRead, "SerialRead", 128, NULL, PRIORITY_HIGHEST, &taskSerialReadHandler);

  //xTaskCreate(TaskSerialWrite, "SerialWrite", 128, NULL, PRIORITY_HIGHEST, &taskSerialWriteHandler);

  //xTaskCreate(TaskHeartbeat, "Heartbeat", 256, NULL, PRIORITY_HIGH, &taskHeartbeatHandler);
  
  xTaskCreate(TaskSensors, "Sensors", 64, NULL, PRIORITY_LOW, &taskSensorsHandler);

  xTaskCreate(TaskLogger, "Logger", 256, NULL, PRIORITY_HIGHEST, &taskLoggerHandler);

  xTaskCreate(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, &taskSdWriteHandler);

  vTaskStartScheduler();
}

void loop() {}
