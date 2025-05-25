#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    taskDISABLE_INTERRUPTS();
    while (!Serial) {}
    Serial.println("Overflow on" + String(pcTaskName) + " task!");
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(2000); 
        digitalWrite(LED_BUILTIN, LOW);
        delay(2000);
    }
}
