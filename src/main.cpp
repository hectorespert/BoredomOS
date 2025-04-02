#include <Arduino.h>
#include <SD.h>
#include <SolarCharger.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <Led.h>

#define SOLAR_CHARGER_READ_MS 1000

#define MAVLINK_HEARTBEAT 1000

TaskHandle_t taskLedHandler = NULL;

File logFile;

SolarCharger solarCharger(A0);

float batteryVoltage = 0.0;

void setup()
{
  Serial.begin(9600);

  /*if (!SD.begin(9)) {
    Serial.println("Card failed, or not present");
    while (1);
  }

  logFile = SD.open("datalog.txt", FILE_WRITE);
  if (!logFile) {
    Serial.print("error opening ");
    while (true);
  }*/

  xTaskCreate(TaskLed, "HealthCheck", 64, NULL, PRIORITY_LOWEST, &taskLedHandler);
}

/*

void readBatteryVoltage()
{
  if (!solarChargerDelay.justFinished()) {
    return;
  }

  solarChargerDelay.repeat();

  batteryVoltage = solarCharger.readVoltage();
}*/

void loop()
{
  //readBatteryVoltage();
}
