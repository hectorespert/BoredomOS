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
#include <RTC.h>
#include <Battery.h>

RTC_DS1307 rtc;

Battery battery = Battery();

TaskHandle_t taskStatusHandler = NULL;

TaskHandle_t taskSensorsHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

TaskHandle_t taskHeartbeatHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

QueueHandle_t sdWriteQueue = NULL;

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskMavlinkBatteryStatus(void *pvParameters);

[[noreturn]] extern void TaskMavlink(void *pvParameters);

void setup()
{
  configASSERT(rtc.begin());
  configASSERT(RTC.begin());

  RTCTime currentTime(rtc.now().unixtime());
  RTC.setTime(currentTime);

  Serial.begin(115200);

  configASSERT(SD.begin(9));

  sdWriteQueue = xQueueCreate(2, sizeof(Log*));
  configASSERT(sdWriteQueue != NULL);

  serialReadQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialReadQueue != NULL);

  serialWriteQueue = xQueueCreate(16, sizeof(mavlink_message_t*));
  configASSERT(serialWriteQueue != NULL);

  xTaskCreate(TaskSerialRead, "SerialRead", 96, NULL, PRIORITY_HIGHEST, &taskSerialReadHandler);

  xTaskCreate(TaskSerialWrite, "SerialWrite", 192, NULL, PRIORITY_HIGHEST, &taskSerialWriteHandler);

  xTaskCreate(TaskHeartbeat, "Heartbeat", 128, NULL, PRIORITY_HIGH, &taskHeartbeatHandler);

  xTaskCreate(TaskMavlink, "Mavlink", 256, NULL, PRIORITY_LOW, &taskMavlinkHandler);
  
  xTaskCreate(TaskSensors, "Sensors", 64, NULL, PRIORITY_LOW, &taskSensorsHandler);

  xTaskCreate(TaskLogger, "Logger", 96, NULL, PRIORITY_HIGH, &taskLoggerHandler);

  xTaskCreate(TaskMavlinkBatteryStatus, "MavlinkBatteryStatus", 128, NULL, PRIORITY_HIGH, &taskStatusHandler);

  xTaskCreate(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, &taskSdWriteHandler);

  vTaskStartScheduler();
}

void loop() {}
