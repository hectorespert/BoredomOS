#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>
#include <RTC.h>
#include <Log.h>
#include <RTClib.h>
#include <Battery.h>

extern QueueHandle_t serialWriteQueue;

static void waitSerial()
{
    while (!Serial) {
        vTaskDelay(125 / portTICK_PERIOD_MS);
    }
}

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

    uint64_t time_unix_usec = currentTime.getUnixTime() * 1000000ULL;

    uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    mavlink_msg_system_time_pack(
        1, 
        MAV_COMP_ID_AUTOPILOT1, 
        systemTimeMsg, 
        time_unix_usec,
        boot_ms
    );

    if (xQueueSend(serialWriteQueue, &systemTimeMsg, 0) != pdPASS)
    {
        vPortFree(systemTimeMsg);
    }
}

static void sendStatusText(const char* text, uint8_t severity)
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

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        waitSerial();

        sendHeartbeat();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);

        waitSerial();

        sendSystemTime();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);
    }
}

extern Battery battery;

static void sendBatteryStatus()
{
    mavlink_message_t* batteryMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

    uint16_t voltages[10];
    voltages[0] = battery.millivolts();
    for (int i = 1; i < 10; ++i) voltages[i] = UINT16_MAX;

    uint16_t voltages_ext[4] = {0, 0, 0, 0};

    mavlink_msg_battery_status_pack(
        1, 
        MAV_COMP_ID_AUTOPILOT1, 
        batteryMsg, 
        0,
        MAV_BATTERY_FUNCTION_ALL,
        MAV_BATTERY_TYPE_LIPO,
        INT16_MAX,
        voltages,
        -1,
        -1,
        -1,
        battery.remaining(),
        0,
        MAV_BATTERY_CHARGE_STATE_UNDEFINED,
        voltages_ext, 
        MAV_BATTERY_MODE_UNKNOWN,
        0
    );

    if (xQueueSend(serialWriteQueue, &batteryMsg, 0) != pdPASS)
    {
        vPortFree(batteryMsg);
    }
}

[[noreturn]] void TaskMavlinkBatteryStatus(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        waitSerial();

        sendBatteryStatus();
        vTaskDelayUntil(&xLastWakeTime, 2000 / portTICK_PERIOD_MS);
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
                    mavlink_command_long_t command;
                    mavlink_msg_command_long_decode(msg, &command);

                    if (command.command == MAV_CMD_GET_HOME_POSITION) {
                        break;
                    }

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

                        RTCTime currentTime;
                        RTC.getTime(currentTime);

                        mavlink_message_t* timeSyncMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));

                        mavlink_msg_timesync_pack(
                            1, 
                            MAV_COMP_ID_AUTOPILOT1,
                            timeSyncMsg,
                            currentTime.getUnixTime() * 1000000000ULL,
                            timesync.ts1,
                            timesync.target_system,
                            timesync.target_component
                        );

                        if (xQueueSend(serialWriteQueue, &timeSyncMsg, 0) != pdPASS) {
                            vPortFree(timeSyncMsg);
                        }
                    }

                    break;
                }
        
                default:
                    sendStatusText("Mensaje recibido con ID desconocido: " + msg->msgid, MAV_SEVERITY_WARNING);
                    break;
            }

            vPortFree(msg);
        }
    }

}