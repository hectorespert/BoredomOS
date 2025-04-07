#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialQueue;

[[noreturn]] void onSerialReceive()
{
    while (Serial.available() > 0)
    {
        uint8_t c = Serial.read();
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xQueueSendFromISR(serialQueue, &c, &xHigherPriorityTaskWoken);

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static mavlink_message_t msg;
static uint8_t buf[MAVLINK_MAX_PACKET_LEN];

[[noreturn]] void TaskSerialWrite(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        mavlink_msg_heartbeat_pack(1, MAV_COMP_ID_AUTOPILOT1, &msg, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_GENERIC, MAV_MODE_FLAG_MANUAL_INPUT_ENABLED, 0, MAV_STATE_STANDBY);
        uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);

        Serial.write(buf, len);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static mavlink_message_t readed_msg;
static mavlink_status_t status;

[[noreturn]] void TaskSerialRead(void *pvParameters)
{
    (void) pvParameters;
    uint8_t receivedByte;

    for (;;)
    {
        // Esperar un byte en la cola
        if (xQueueReceive(serialQueue, &receivedByte, portMAX_DELAY))
        {
            // Intentar decodificar el mensaje MAVLink
            if (mavlink_parse_char(MAVLINK_COMM_0, receivedByte, &readed_msg, &status))
            {
                // Mensaje MAVLink recibido correctamente
                //Serial.print("Mensaje MAVLink recibido, ID: ");
                //Serial.println(readed_msg.msgid);

                // Procesar el mensaje según su ID
                switch (readed_msg.msgid)
                {
                case MAVLINK_MSG_ID_HEARTBEAT:
                    //Serial.println("Mensaje Heartbeat recibido");
                    break;

                case MAVLINK_MSG_ID_ATTITUDE:
                    //Serial.println("Mensaje Attitude recibido");
                    break;

                default:
                    //Serial.println("Mensaje no reconocido");
                    break;
                }
            }
        }
    }
}
