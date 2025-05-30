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
            vPortFree(msg_to_send);
            Serial.write(buf, len);
        }
    }
}

extern QueueHandle_t serialReadQueue;

static mavlink_message_t msg_to_read;
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

            if (mavlink_parse_char(MAVLINK_COMM_0, receivedByte, &msg_to_read, &status)) {

                mavlink_message_t* readed_msg = (mavlink_message_t*) pvPortMalloc(sizeof(mavlink_message_t));
                if (readed_msg != NULL)
                {
                    memcpy(readed_msg, &msg_to_read, sizeof(mavlink_message_t));
                    
                    if (xQueueSend(serialReadQueue, &readed_msg, 0) != pdPASS) {
                        vPortFree(readed_msg);
                    }
                }
            }       
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
