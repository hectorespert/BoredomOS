#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MavlinkShared.h>
#include <Battery.h>
#include <SystemTime.h>
#include <Recovery.h>
#include <string.h>

extern QueueHandle_t serialWriteQueue;

extern SystemTime systemTime;

extern Battery battery;

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

// ---------------------------------------------------------------------------
// The four shared builders (include/MavlinkShared.h). Each fills the
// mavlink_message_t the caller provides -- see design.md, "The message logic
// is shared with src/mavlink.cpp, not duplicated" -- so both Serial1's
// allocate-and-queue path below and the USB endpoint's pack-and-write path
// call the exact same packing code with the exact same identity triple.
// ---------------------------------------------------------------------------

void mavlinkBuildHeartbeat(mavlink_message_t *out, uint8_t chan)
{
    uint8_t baseMode = MAV_MODE_FLAG_SAFETY_ARMED;
    if (!reducedConfiguration) {
        baseMode |= MAV_MODE_FLAG_AUTO_ENABLED;
    }

    mavlink_msg_heartbeat_pack_chan(
        1,
        MAV_COMP_ID_AUTOPILOT1,
        chan,
        out,
        MAV_TYPE_ROCKET,
        MAV_AUTOPILOT_GENERIC,
        baseMode,
        packCustomMode(),
        reducedConfiguration ? MAV_STATE_CRITICAL : MAV_STATE_ACTIVE
    );
}

void mavlinkBuildSystemTime(mavlink_message_t *out, uint8_t chan)
{
    uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    mavlink_msg_system_time_pack_chan(
        1,
        MAV_COMP_ID_AUTOPILOT1,
        chan,
        out,
        systemTime.getUnixTimeUsec(),
        boot_ms
    );
}

void mavlinkBuildBatteryStatus(mavlink_message_t *out, uint8_t chan)
{
    uint16_t voltages[10];
    voltages[0] = battery.millivolts();
    for (int i = 1; i < 10; ++i) voltages[i] = UINT16_MAX;

    uint16_t voltages_ext[4] = {0, 0, 0, 0};

    mavlink_msg_battery_status_pack_chan(
        1,
        MAV_COMP_ID_AUTOPILOT1,
        chan,
        out,
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
}

void mavlinkBuildStatusText(mavlink_message_t *out, uint8_t chan, uint8_t severity, const char *text)
{
    mavlink_msg_statustext_pack_chan(
        1,
        MAV_COMP_ID_AUTOPILOT1,
        chan,
        out,
        severity,
        text,
        0,
        0
    );
}

// ---------------------------------------------------------------------------
// Serial1's producers: allocate, build, queue -- the pattern
// ARCHITECTURE.md's queue memory ownership protocol requires. Unchanged in
// shape from before this refactor; only the packing itself moved above.
// ---------------------------------------------------------------------------

static void sendHeartbeat() {
    mavlink_message_t* heartbeatMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (heartbeatMsg != NULL) {
        mavlinkBuildHeartbeat(heartbeatMsg, MAVLINK_COMM_0);

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
        mavlinkBuildSystemTime(systemTimeMsg, MAVLINK_COMM_0);

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
        mavlinkBuildStatusText(statusMsg, MAVLINK_COMM_0, severity, text);
        if (xQueueSend(serialWriteQueue, &statusMsg, 0) != pdPASS) {
            vPortFree(statusMsg);
        }
    }
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

static void sendBatteryStatus()
{
    mavlink_message_t* batteryMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (batteryMsg != NULL) {
        mavlinkBuildBatteryStatus(batteryMsg, MAVLINK_COMM_0);

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

// ---------------------------------------------------------------------------
// The shared inbound dispatch (include/MavlinkShared.h). Both Serial1's
// TaskMavlink below and the USB endpoint call this for every message they
// parse; neither keeps its own copy of what a message means -- see design.md.
//
// rebootRequested is set, not acted on, when a message commands one: the
// caller owns its own transport's timing (Serial1 queues and needs a moment
// to drain; USB writes synchronously and does not), so only the caller knows
// when it is safe to reset after the reply -- review.md's PRCR/timing notes,
// carried forward from before this refactor rather than re-derived.
//
// INVARIANT a caller MAY rely on: msg and reply may point to the SAME
// mavlink_message_t. src/cli.cpp does exactly this (one static buffer for
// both directions, to afford a second 291-byte item on an already-tight RAM
// budget -- see its own comment). This holds only because every case below
// fully extracts what it needs from msg -- into a local struct
// (mavlink_command_long_t, mavlink_timesync_t) or a single scalar read --
// before the first write to reply in that same case. A case that reads msg
// again after writing reply would read its own output instead and silently
// break this. Serial1's caller does not alias them (msg is a heap pointer
// freed separately from reply's own lifetime), so this invariant is
// USB-specific, not a general property callers must exploit.
// ---------------------------------------------------------------------------

bool mavlinkHandleInbound(const mavlink_message_t *msg, uint8_t replyChan, mavlink_message_t *reply, bool *rebootRequested)
{
    *rebootRequested = false;

    switch (msg->msgid)
    {
        case MAVLINK_MSG_ID_HEARTBEAT:
            return false;

        case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
            return false;

        case MAVLINK_MSG_ID_COMMAND_LONG: {
            mavlink_command_long_t command;
            mavlink_msg_command_long_decode(msg, &command);

            if (command.command == MAV_CMD_GET_HOME_POSITION) {
                return false;
            }

            if (command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) {
                // Acknowledged rather than silently handled -- review.md
                // finding 17. This is a new message on the wire; the
                // closed-set HIL case (test/test_hil/check_recovery.py,
                // task 7.5) must expect it.
                mavlink_msg_command_ack_pack_chan(
                    1,
                    MAV_COMP_ID_AUTOPILOT1,
                    replyChan,
                    reply,
                    command.command,
                    MAV_RESULT_ACCEPTED,
                    0,
                    0,
                    0,
                    0
                );

                // reinitialise() clears the deliberate-reset field along
                // with both counters and the snapshot, so it has to run
                // *before* the marker is set, not after -- otherwise it
                // would wipe the very marker that stops this reboot from
                // counting as evidence of a fault (design.md's
                // deliberate-reset carve-out, review.md finding 4).
                Recovery::reinitialise();
                Recovery::setDeliberateReset(Recovery::DeliberateReset::Commanded);

                *rebootRequested = true;
                return true;
            }

            return false;
        }

        case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
            return false;

        case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:
            return false;

        case MAVLINK_MSG_ID_SYSTEM_TIME: {
            time_t unix_time_from_gcs = mavlink_msg_system_time_get_time_unix_usec(msg) / USEC_PER_SEC;
            systemTime.setUnixTime(unix_time_from_gcs);
            return false;
        }

        case MAVLINK_MSG_ID_TIMESYNC: {
            mavlink_timesync_t timesync;
            mavlink_msg_timesync_decode(msg, &timesync);

            if (timesync.tc1 == 0) {
                mavlink_msg_timesync_pack_chan(
                    1,
                    MAV_COMP_ID_AUTOPILOT1,
                    replyChan,
                    reply,
                    systemTime.getUnixTimeNsec(),
                    timesync.ts1,
                    timesync.target_system,
                    timesync.target_component
                );
                return true;
            }

            return false;
        }

        default:
            // Same pointer-arithmetic defect as before this refactor --
            // TODO.md's "Fix the pointer arithmetic in the unknown-message
            // STATUSTEXT" entry already tracks it, and preserving it
            // unchanged here is what keeps this section's Serial1 behaviour
            // byte-identical (task 2.4). It is now reachable from the USB
            // endpoint too, once section 3/4 wires that path in; noted in
            // that entry rather than fixed as a drive-by here.
            mavlinkBuildStatusText(reply, replyChan, MAV_SEVERITY_WARNING, "Mensaje recibido con ID desconocido: " + msg->msgid);
            return true;
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
            mavlink_message_t reply;
            bool rebootRequested = false;
            bool hasReply = mavlinkHandleInbound(msg, MAVLINK_COMM_0, &reply, &rebootRequested);

            vPortFree(msg);

            if (hasReply) {
                mavlink_message_t* replyMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
                if (replyMsg != NULL) {
                    memcpy(replyMsg, &reply, sizeof(mavlink_message_t));
                    if (xQueueSend(serialWriteQueue, &replyMsg, 0) != pdPASS) {
                        vPortFree(replyMsg);
                    }
                }
            }

            if (rebootRequested) {
                // Gives TaskSerialWrite, which is higher priority than this
                // task, a chance to drain the ack onto the link before the
                // reset -- see review.md's PRCR/timing notes.
                vTaskDelay(pdMS_TO_TICKS(50));
                NVIC_SystemReset();
            }
        }
    }

}
