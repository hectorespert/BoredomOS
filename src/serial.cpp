#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>
#include <Link.h>
#include <LinkMsg.h>
#include <MavlinkPack.h>

extern QueueHandle_t serialWriteQueue;

[[noreturn]] void TaskSerialWrite(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        LinkMsg intent;
        if (xQueueReceive(serialWriteQueue, &intent, portMAX_DELAY))
        {
            mavlink_message_t msg_to_send;
            mavlinkPack(intent, &msg_to_send);

            uint8_t buf[MAVLINK_MAX_PACKET_LEN];
            uint16_t len = mavlink_msg_to_send_buffer(buf, &msg_to_send);
            LINK_SERIAL.write(buf, len);
        }
    }
}

extern QueueHandle_t serialReadQueue;

static mavlink_message_t msg_to_read;
static mavlink_status_t status;

[[noreturn]] void TaskSerialRead(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        while (LINK_SERIAL.available() > 0)
        {
            uint8_t receivedByte = LINK_SERIAL.read();

            if (mavlink_parse_char(MAVLINK_COMM_0, receivedByte, &msg_to_read, &status)) {
                xQueueSend(serialReadQueue, &msg_to_read, 0);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
