#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialWriteQueue;

[[noreturn]] void TaskSerialWrite(void *pvParameters)
{
    (void) pvParameters;

    while (!Serial) {
        vTaskDelay(125 / portTICK_PERIOD_MS);
    }

    for (;;)
    {
        mavlink_message_t* msg_to_send;
        if (xQueueReceive(serialWriteQueue, &msg_to_send, portMAX_DELAY))
        {
            uint8_t buf[MAVLINK_MAX_PACKET_LEN];
            uint16_t len = mavlink_msg_to_send_buffer(buf, msg_to_send);
            Serial.write(buf, len);
            vPortFree(msg_to_send);
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
        vTaskDelay(125 / portTICK_PERIOD_MS);
    }

    for (;;)
    {
        while (Serial.available() > 0)
        {
            uint8_t receivedByte = Serial.read();

            Serial.print("Received byte: ");
            Serial.println(receivedByte);

            /*if (mavlink_parse_char(MAVLINK_COMM_0, receivedByte, &readed_msg, &status))
            {
                Serial.print("Received message: ");
                Serial.println(readed_msg.msgid);
                Serial.print("Message length: ");
                Serial.println(readed_msg.len);
                Serial.print("Message checksum: ");
                Serial.println(readed_msg.checksum);

                //xQueueSend(serialReadQueue, &readed_msg, 0);
            }*/

            
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
