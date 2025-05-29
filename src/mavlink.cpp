#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>
#include <RTC.h>
#include <Log.h>
#include <RTClib.h>

extern QueueHandle_t serialWriteQueue;

static void sendHeartbeat() {
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
}

static void sendSystemTime()
{
    mavlink_message_t* systemTimeMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

    RTCTime currentTime;
    RTC.getTime(currentTime);

    uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    mavlink_msg_system_time_pack(
        1, 
        MAV_COMP_ID_AUTOPILOT1, 
        systemTimeMsg, 
        currentTime.getUnixTime() * 1000000ULL, 
        boot_ms
    );

    if (xQueueSend(serialWriteQueue, &systemTimeMsg, 0) != pdPASS)
    {
        vPortFree(systemTimeMsg);
    }
}

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (!Serial) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for (;;)
    {
        sendHeartbeat();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);

        sendSystemTime();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);
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
extern RTC_DS1307 rtc;

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

                case MAVLINK_MSG_ID_SYSTEM_TIME: {
                    time_t unix_time_from_gcs = mavlink_msg_system_time_get_time_unix_usec(msg) / 1000000ULL; 

                    RTCTime currentTime;
                    RTC.getTime(currentTime);

                    if (currentTime.getUnixTime() == unix_time_from_gcs) {
                        break;
                    }

                    currentTime.setUnixTime(unix_time_from_gcs);
                    RTC.setTime(currentTime);

                    if (rtc.now().unixtime() == currentTime.getUnixTime()) {
                        break;
                    }

                    rtc.adjust(DateTime(currentTime.getUnixTime()));

                    break;
                }

                case MAVLINK_MSG_ID_TIMESYNC: {
                    mavlink_timesync_t timesync;
                    mavlink_msg_timesync_decode(msg, &timesync);

                    if (timesync.tc1 == 0) {

                        mavlink_message_t* timeSyncMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

                        RTCTime currentTime;
                        RTC.getTime(currentTime);

                        mavlink_msg_timesync_pack(
                            1, 
                            MAV_COMP_ID_AUTOPILOT1, 
                            timeSyncMsg, 
                            currentTime.getUnixTime() * 1000ULL, 
                            timesync.ts1, 
                            timesync.target_system, 
                            timesync.target_component
                        );

                        if (xQueueSend(serialWriteQueue, &timeSyncMsg, 0) != pdPASS) {
                            vPortFree(timeSyncMsg);
                        }

                    } else {
                        Serial.print("Received time sync response: ");
                        Serial.print("tc1: ");
                        Serial.print(timesync.tc1);
                        Serial.print(", ts1: ");
                        Serial.print(timesync.ts1);
                        Serial.print(", target_system: ");
                        Serial.print(timesync.target_system);
                        Serial.print(", target_component: ");
                        Serial.println(timesync.target_component);
                    }
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