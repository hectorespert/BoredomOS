#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>
#include <RTC.h>
#include <Log.h>

extern QueueHandle_t serialWriteQueue;

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for (;;)
    {

        mavlink_message_t* heartbeatMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

        mavlink_msg_heartbeat_pack(
            1, 
            MAV_COMP_ID_AUTOPILOT1, 
            heartbeatMsg, 
            MAV_TYPE_ROCKET, 
            MAV_AUTOPILOT_GENERIC, 
            MAV_MODE_FLAG_AUTO_ENABLED, 
            0, 
            MAV_STATE_ACTIVE
        );

        if (xQueueSend(serialWriteQueue, &heartbeatMsg, 0) != pdPASS)
        {
            vPortFree(heartbeatMsg);
        }

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }
}

void sendStatusText(const char* text, uint8_t severity)
{
    mavlink_message_t* statusMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (statusMsg != NULL) {
        mavlink_msg_statustext_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            statusMsg,
            severity,
            text,
            0,
            0
        );
        if (xQueueSend(serialWriteQueue, &statusMsg, 0) != pdPASS) {
            vPortFree(statusMsg);
        }
    }
}

extern QueueHandle_t mavlinkStatusQueue;

[[noreturn]] void TaskStatus(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        Log* log;
        if (xQueueReceive(mavlinkStatusQueue, &log, portMAX_DELAY) == pdPASS) {
        
            String text = String(log->unixtime) + 
            " " + log->system.heap + 
            " " + log->system.taskLoggerAvailableStack +
            " " + log->system.taskHeartbeatAvailableStack +
            " " + log->system.taskSensorsAvailableStack +
            " " + log->system.taskStatusAvailableStack +
            " " + log->system.taskSdWriteAvailableStack +
            " " + log->system.taskMavlinkAvailableStack +
            " " + log->system.taskSerialReadAvailableStack +
            " " + log->system.taskSerialWriteAvailableStack;
            vPortFree(log);
            sendStatusText(text.c_str(), MAV_SEVERITY_INFO);
        }
    }
}

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
                    Serial.println("SYSTEM_TIME: ");
                    Serial.print("Time since system boot: ");
                    Serial.println(mavlink_msg_system_time_get_time_boot_ms(msg));
                    Serial.print("Time since UNIX epoch: ");
                    Serial.println(mavlink_msg_system_time_get_time_unix_usec(msg));
                    break;

                case MAVLINK_MSG_ID_TIMESYNC: {
                    Serial.println("TIMESYNC: ");
                    Serial.print("Component ID: ");
                    Serial.println(msg->compid);
                    Serial.print("System ID: ");
                    Serial.println(msg->sysid);
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