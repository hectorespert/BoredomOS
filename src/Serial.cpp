#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

[[noreturn]] void TaskSerial(void *pvParameters)
{
    (void) pvParameters;

    uint32_t notificationValue;

    Serial.begin(9600);


    for (;;)
    {
        xTaskNotifyWait(0x00, 0x00, &notificationValue, portMAX_DELAY);

        float voltage = *(float*)&notificationValue;

        Serial.print("Voltage: ");
        Serial.println(voltage);
    }

}