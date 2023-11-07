#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "healthcheck.h"
#include "priority.h"

void setup()
{
    xTaskCreate(TaskHealthCheck, "Led", 128, nullptr, PRIORITY_HIGHEST, nullptr);
}

void loop()
{
}
