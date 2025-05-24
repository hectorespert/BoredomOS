#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>

extern QueueHandle_t serialReadQueue;

[[noreturn]] void TaskMavlink(void *pvParameters)
{
    (void)pvParameters;


    for (;;)
    {
        mavlink_message_t* msg;
        if (xQueueReceive(serialReadQueue, &msg, portMAX_DELAY))
        {

            switch (msg->msgid)
            {
                case MAVLINK_MSG_ID_HEARTBEAT:
                    break;

                case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
                    break;

                case MAVLINK_MSG_ID_COMMAND_LONG:
                    break;

                case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
                    break;

                case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:
                    break;

                case MAVLINK_MSG_ID_SYSTEM_TIME:
                    mavlink_system_time_t sys_time;
                    mavlink_msg_system_time_decode(msg, &sys_time);

                    Serial.print("SYSTEM_TIME: time_boot_ms = ");
                    Serial.print(sys_time.time_boot_ms);
                    Serial.print(" ms, time_unix_usec = ");
                    Serial.print((uint32_t)(sys_time.time_unix_usec / 1000000ULL));
                    Serial.println(" s");
                    break;

                case MAVLINK_MSG_ID_TIMESYNC: {
                    Serial.println("TIMESYNC: ");
                    break;
                }
        
                default:
                    Serial.print("Mensaje recibido con ID desconocido: ");
                    Serial.println(msg->msgid);
                    break;
            }

            vPortFree(msg);
        }
    }

}