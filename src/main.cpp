#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <Led.h>
#include <Sensors.h>
#include <Wire.h>
#include <RTClib.h>
#include <Serial.h>

#define MAVLINK_HEARTBEAT 1000

TaskHandle_t taskLedHandler = NULL;

TaskHandle_t taskSensorsHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

QueueHandle_t serialQueue;

File logFile;

RTC_DS1307 rtc;

void setup()
{
  Serial.begin(115200);


  if (!rtc.begin()) {
    while (1);
  }

  //rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  /*if (!SD.begin(9)) {
    Serial.println("Card failed, or not present");
    while (1);
  }

  logFile = SD.open("datalog.txt", FILE_WRITE);
  if (!logFile) {
    Serial.print("error opening ");
    while (true);
  }*/

  serialQueue = xQueueCreate(64, sizeof(uint8_t));

  attachInterrupt(digitalPinToInterrupt(0), onSerialReceive, RISING);

  xTaskCreate(TaskLed, "Led", 56, NULL, PRIORITY_LOW, &taskLedHandler);

  xTaskCreate(TaskSerialRead, "SerialRead", 128, NULL, PRIORITY_HIGHEST, &taskSerialReadHandler);

  xTaskCreate(TaskSerialWrite, "SerialWrite", 128, NULL, PRIORITY_HIGHEST, &taskSerialWriteHandler);

  xTaskCreate(TaskSensors, "Sensors", 48, NULL, PRIORITY_HIGH, &taskSensorsHandler);

  vTaskStartScheduler();
}

void loop() {}
