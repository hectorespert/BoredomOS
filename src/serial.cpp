#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialWriteQueue;

static mavlink_message_t msg_to_send;
static uint8_t buf[MAVLINK_MAX_PACKET_LEN];

[[noreturn]] void TaskSerialWrite(void *pvParameters)
{
    (void) pvParameters;

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for (;;)
    {
        if (xQueueReceive(serialWriteQueue, &msg_to_send, portMAX_DELAY))
        {
            Serial.println("Sending heartbeat... asdasda");
            uint16_t len = mavlink_msg_to_send_buffer(buf, &msg_to_send);
            Serial.write(buf, len);
        }
    }
}

extern QueueHandle_t serialReadQueue;

static mavlink_message_t readed_msg;
static mavlink_status_t status;

[[noreturn]] void TaskSerialRead(void *pvParameters)
{
    (void) pvParameters;

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    uint8_t receivedByte;

    for (;;)
    {
        while (Serial.available() > 0)
        {
            receivedByte = Serial.read();

            if (mavlink_parse_char(MAVLINK_COMM_0, receivedByte, &readed_msg, &status))
            {
                xQueueSend(serialReadQueue, &readed_msg, 0);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
