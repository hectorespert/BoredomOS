#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Battery.h>
#include <SystemTime.h>
#include <Recovery.h>
#include <string.h>

extern QueueHandle_t serialWriteQueue;

extern SystemTime systemTime;

extern bool reducedConfiguration;
extern Recovery::ResetReason previousResetReason;
extern Recovery::BootPhase previousBootPhase;

// How long the firmware must run before a boot counts as stable, and how long
// the reduced configuration waits before trying the normal one again. Both are
// derived in design.md from the heap's worst-case leak rate, not guessed
// (review.md finding 15).
constexpr uint32_t STABILITY_WINDOW_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t RETRY_INTERVAL_MS = 30UL * 60UL * 1000UL;

// custom_mode is a uint32_t this firmware sent as zero before this change. It now
// carries, one byte each, the reset reason, the phase the previous boot reached,
// and both counters clamped to their thresholds for display -- the stored counts
// themselves are not clamped (see include/Recovery.h) so the automatic retry can
// still tell its own contribution apart from a further fault. See design.md.
static uint32_t packCustomMode()
{
    uint8_t consecutive = Recovery::getConsecutiveCount();
    if (consecutive > Recovery::CONSECUTIVE_THRESHOLD) consecutive = Recovery::CONSECUTIVE_THRESHOLD;
    uint8_t cumulative = Recovery::getCumulativeCount();
    if (cumulative > Recovery::CUMULATIVE_THRESHOLD) cumulative = Recovery::CUMULATIVE_THRESHOLD;

    return (uint32_t)previousResetReason
        | ((uint32_t)previousBootPhase << 8)
        | ((uint32_t)consecutive << 16)
        | ((uint32_t)cumulative << 24);
}

static void sendHeartbeat() {
    mavlink_message_t* heartbeatMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (heartbeatMsg != NULL) {
        uint8_t baseMode = MAV_MODE_FLAG_SAFETY_ARMED;
        if (!reducedConfiguration) {
            baseMode |= MAV_MODE_FLAG_AUTO_ENABLED;
        }

        mavlink_msg_heartbeat_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            heartbeatMsg,
            MAV_TYPE_ROCKET,
            MAV_AUTOPILOT_GENERIC,
            baseMode,
            packCustomMode(),
            reducedConfiguration ? MAV_STATE_CRITICAL : MAV_STATE_ACTIVE
        );

        if (xQueueSend(serialWriteQueue, &heartbeatMsg, 0) != pdPASS)
        {
            vPortFree(heartbeatMsg);
        }
    }
}

static void sendSystemTime()
{
    mavlink_message_t* systemTimeMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (systemTimeMsg != NULL) {
        uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        mavlink_msg_system_time_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            systemTimeMsg,
            systemTime.getUnixTimeUsec(),
            boot_ms
        );

        if (xQueueSend(serialWriteQueue, &systemTimeMsg, 0) != pdPASS)
        {
            vPortFree(systemTimeMsg);
        }
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

static const char* resetReasonText(Recovery::ResetReason reason)
{
    switch (reason) {
        case Recovery::ResetReason::PowerOn: return "power-on";
        case Recovery::ResetReason::LowVoltage: return "low voltage";
        case Recovery::ResetReason::Watchdog: return "watchdog";
        case Recovery::ResetReason::Software: return "software";
        case Recovery::ResetReason::ExternalUnknown: return "external/unknown";
        case Recovery::ResetReason::BackupStateInvalid: return "backup state invalid";
    }
    return "unknown";
}

static const char* bootPhaseText(Recovery::BootPhase phase)
{
    switch (phase) {
        case Recovery::BootPhase::Start: return "start";
        case Recovery::BootPhase::LinkDone: return "link";
        case Recovery::BootPhase::ClockDone: return "clock";
        case Recovery::BootPhase::CardDone: return "card";
        case Recovery::BootPhase::QueuesDone: return "queues";
        case Recovery::BootPhase::TasksDone: return "tasks";
        case Recovery::BootPhase::SchedulerStarted: return "scheduler";
        case Recovery::BootPhase::StackOverflowFault: return "stack overflow";
        case Recovery::BootPhase::MallocFailedFault: return "malloc failed";
    }
    return "unknown";
}

// Longest combination is 48 characters ("Reset: backup state invalid, phase
// malloc failed"), under the 50-byte STATUSTEXT text field -- no printf family
// involved, per review.md's note that newlib-nano's vsnprintf costs stack this
// task's 128 words does not have to spare.
static void sendBootStatusText()
{
    char text[50];
    text[0] = '\0';
    strncat(text, "Reset: ", sizeof(text) - 1);
    strncat(text, resetReasonText(previousResetReason), sizeof(text) - 1 - strlen(text));
    strncat(text, ", phase ", sizeof(text) - 1 - strlen(text));
    strncat(text, bootPhaseText(previousBootPhase), sizeof(text) - 1 - strlen(text));

    sendStatusText(text, MAV_SEVERITY_CRITICAL);
}

static void sendCommandAck(uint16_t command, uint8_t result)
{
    mavlink_message_t* ackMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (ackMsg != NULL) {
        mavlink_msg_command_ack_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            ackMsg,
            command,
            result,
            0,
            0,
            0,
            0
        );
        if (xQueueSend(serialWriteQueue, &ackMsg, 0) != pdPASS) {
            vPortFree(ackMsg);
        }
    }
}

[[noreturn]] void TaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;

    sendBootStatusText();

    TickType_t bootTick = xTaskGetTickCount();
    bool stabilityCleared = false;
    bool retryAttempted = false;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        sendHeartbeat();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);

        sendSystemTime();

        vTaskDelayUntil(&xLastWakeTime, 500 / portTICK_PERIOD_MS);

        // Both live here because TaskHeartbeat is the only task that runs in
        // every configuration -- reduced included -- so it is the only place
        // that can be trusted to ever clear the counter or fire the retry
        // (review.md finding 16).
        uint32_t elapsedMs = (xTaskGetTickCount() - bootTick) * portTICK_PERIOD_MS;

        if (!stabilityCleared && elapsedMs >= STABILITY_WINDOW_MS) {
            Recovery::setConsecutiveCount(0);
            stabilityCleared = true;
        }

        if (reducedConfiguration && !retryAttempted && elapsedMs >= RETRY_INTERVAL_MS) {
            retryAttempted = true;
            Recovery::setDeliberateReset(Recovery::DeliberateReset::Retry);
            NVIC_SystemReset();
        }
    }
}

extern Battery battery;

static void sendBatteryStatus()
{
    mavlink_message_t* batteryMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (batteryMsg != NULL) {
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
}

[[noreturn]] void TaskMavlinkBatteryStatus(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
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

                    if (command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) {
                        // Acknowledged rather than silently handled -- review.md
                        // finding 17. This is a new message on the wire; the
                        // closed-set HIL case (test/test_hil/check_recovery.py,
                        // task 7.5) must expect it.
                        sendCommandAck(command.command, MAV_RESULT_ACCEPTED);

                        // reinitialise() clears the deliberate-reset field along
                        // with both counters and the snapshot, so it has to run
                        // *before* the marker is set, not after -- otherwise it
                        // would wipe the very marker that stops this reboot from
                        // counting as evidence of a fault (design.md's
                        // deliberate-reset carve-out, review.md finding 4).
                        Recovery::reinitialise();
                        Recovery::setDeliberateReset(Recovery::DeliberateReset::Commanded);

                        // Gives TaskSerialWrite, which is higher priority than
                        // this task, a chance to drain the ack onto the link
                        // before the reset -- see review.md's PRCR/timing notes.
                        vTaskDelay(pdMS_TO_TICKS(50));

                        NVIC_SystemReset();
                    }

                    break;

                case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
                    break;

                case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:
                    break;

                case MAVLINK_MSG_ID_SYSTEM_TIME: {
                    time_t unix_time_from_gcs = mavlink_msg_system_time_get_time_unix_usec(msg) / USEC_PER_SEC;
                    systemTime.setUnixTime(unix_time_from_gcs);
                    break;
                }

                case MAVLINK_MSG_ID_TIMESYNC: {
                    mavlink_timesync_t timesync;
                    mavlink_msg_timesync_decode(msg, &timesync);

                    if (timesync.tc1 == 0) {
                        mavlink_message_t* timeSyncMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
                        if (timeSyncMsg != NULL) {
                            mavlink_msg_timesync_pack(
                                1,
                                MAV_COMP_ID_AUTOPILOT1,
                                timeSyncMsg,
                                systemTime.getUnixTimeNsec(),
                                timesync.ts1,
                                timesync.target_system,
                                timesync.target_component
                            );

                            if (xQueueSend(serialWriteQueue, &timeSyncMsg, 0) != pdPASS) {
                                vPortFree(timeSyncMsg);
                            }
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