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

// Set once in setup(), before the scheduler starts (src/main.cpp:335-357), and
// never reassigned after -- safe to read directly from the housekeeping table
// below rather than snapshotting them into a table built at static-init time,
// which would run before setup() and see every handle still NULL.
extern TaskHandle_t taskSerialReadHandler;
extern TaskHandle_t taskSerialWriteHandler;
extern TaskHandle_t taskMavlinkHandler;
extern TaskHandle_t taskCliHandler;
extern TaskHandle_t taskLoggerHandler;
extern TaskHandle_t taskSdWriteHandler;

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

// Housekeeping telemetry -- free heap, minimum-ever-free heap, and each live
// task's stack high-water mark, published one value per TaskMavlink pass as
// NAMED_VALUE_INT (design.md, "One value per schedule pass, with its own
// round-robin index"). Off by default; armed by MAV_CMD_SET_MESSAGE_INTERVAL
// below.
//
// Every name is a full 10-byte array, not a shorter string literal: the pack
// function's mav_array_memcpy always copies exactly 10 bytes from `name`
// regardless of its actual length, so a literal shorter than 10 bytes (e.g.
// "Cli") would read past the end of it. Declaring these as char[10] makes the
// compiler pad the remainder with zeros instead. "SerialWrite" is 11
// characters and truncates to "SerialWrit" in the 10-byte field -- no other
// task name collides with the truncated form today, and cli.cpp's own
// `ps <name>` already truncates its argument the same way, so this is an
// existing acceptance, not a new one (design.md's Risks).
// 12 bytes, not 10: a char[10] initialized from a 10-character literal (no
// room left for the implicit terminator, e.g. "SerialRead") is a hard error
// under this toolchain's g++ (-fpermissive would only downgrade the
// standard-permitted exact-length case to a warning, and this build does not
// pass that flag). 12 is comfortably more than the 10 bytes the pack
// function ever reads from these, for every name here.
static const char kNameHeapFree[12]    = "HeapFree";
static const char kNameHeapMin[12]     = "HeapMin";
static const char kNameSerialRead[12]  = "SerialRead";
static const char kNameSerialWrite[12] = "SerialWrite";
static const char kNameMavlink[12]     = "Mavlink";
static const char kNameCli[12]         = "Cli";
static const char kNameLogger[12]      = "Logger";
static const char kNameSdWrite[12]     = "SdWrite";

struct HousekeepingTaskEntry {
    TaskHandle_t *handle;
    const char *name;
};

// Points at the six extern handles above rather than copying their values, so
// a NULL check always sees setup()'s actual outcome for this boot. Cycle
// length follows the task count -- a future task added to src/main.cpp needs
// an entry here too, and needs serialWriteQueue's depth (below) re-checked,
// per design.md's Risks ("The cycle length grows every time a task is
// added").
static const HousekeepingTaskEntry kHousekeepingTasks[] = {
    { &taskSerialReadHandler,  kNameSerialRead  },
    { &taskSerialWriteHandler, kNameSerialWrite },
    { &taskMavlinkHandler,     kNameMavlink     },
    { &taskCliHandler,         kNameCli         },
    { &taskLoggerHandler,      kNameLogger      },
    { &taskSdWriteHandler,     kNameSdWrite     },
};

constexpr uint8_t kHousekeepingHeapSlots = 2;
constexpr uint8_t kHousekeepingTaskSlots = sizeof(kHousekeepingTasks) / sizeof(kHousekeepingTasks[0]);
constexpr uint8_t kHousekeepingSlots = kHousekeepingHeapSlots + kHousekeepingTaskSlots;

static uint8_t housekeepingCursor = 0;

// Reads the value at the cursor's current slot and advances, wrapping after
// the last one. A task handle that is NULL in this configuration (the
// reduced configuration or no SD card leave taskLoggerHandler/
// taskSdWriteHandler NULL, src/main.cpp:350-357) is skipped -- the cursor
// keeps advancing within this same call rather than reporting
// uxTaskGetStackHighWaterMark(NULL), which FreeRTOS resolves to "the calling
// task" (TaskMavlink itself), not an error, and would silently mislabel its
// stack mark under the wrong task's name (design.md's Context). Returns
// false only if every slot from the cursor onward is a NULL task handle,
// which cannot happen today -- SerialRead, SerialWrite, Mavlink and Cli are
// unconditional (src/main.cpp:335-348) -- but is handled rather than
// assumed.
static bool nextHousekeepingValue(const char **name, int32_t *value)
{
    for (uint8_t tries = 0; tries < kHousekeepingSlots; ++tries) {
        uint8_t slot = housekeepingCursor;
        housekeepingCursor = (housekeepingCursor + 1) % kHousekeepingSlots;

        if (slot == 0) {
            *name = kNameHeapFree;
            *value = (int32_t)xPortGetFreeHeapSize();
            return true;
        }
        if (slot == 1) {
            *name = kNameHeapMin;
            *value = (int32_t)xPortGetMinimumEverFreeHeapSize();
            return true;
        }

        const HousekeepingTaskEntry &entry = kHousekeepingTasks[slot - kHousekeepingHeapSlots];
        TaskHandle_t handle = *entry.handle;
        if (handle == NULL) continue;

        *name = entry.name;
        *value = (int32_t)uxTaskGetStackHighWaterMark(handle);
        return true;
    }
    return false;
}

// Follows sendHeartbeat/sendSystemTime's shape: pvPortMalloc, NULL check,
// pack, xQueueSend onto serialWriteQueue, vPortFree on a failed send. Exactly
// one value per call -- never a burst of several -- so this entry is
// structurally identical to every other one from the queue's point of view
// (design.md's "one ScheduleEntry function that sends all N values" was
// rejected for exactly this reason).
static void sendHousekeeping()
{
    const char *name;
    int32_t value;
    if (!nextHousekeepingValue(&name, &value)) return;

    mavlink_message_t* housekeepingMsg = (mavlink_message_t*)pvPortMalloc(sizeof(mavlink_message_t));
    if (housekeepingMsg != NULL) {
        uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        mavlink_msg_named_value_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            housekeepingMsg,
            boot_ms,
            name,
            value
        );

        if (xQueueSend(serialWriteQueue, &housekeepingMsg, 0) != pdPASS)
        {
            vPortFree(housekeepingMsg);
        }
    }
}

extern QueueHandle_t serialReadQueue;
extern RTC_DS1307 rtc;

// {function, interval_ms, last_ms, enabled} -- last_ms is the tick (in ms since
// boot) an entry last fired, seeded one interval before TaskMavlink's own start
// (not before absolute tick zero -- Copilot's PR review caught that the earlier
// version anchored to boot, which could fire every entry back-to-back on the
// first pass if setup() ever took longer than an interval) so the first pass
// finds each entry already due. An entry is due once
// (now - last_ms) >= interval_ms; a fired entry advances with
// last_ms += interval_ms rather than to "now", so a late pass does not push
// the cadence forward -- this is what replaces vTaskDelayUntil's drift-free
// property now that three producers share one task instead of two dedicated
// ones.
//
// Both comparisons above use "now - last_ms" (an unsigned delta from a fixed
// reference), never "now >= last_ms + interval_ms" (comparing two absolute,
// independently-drifting values). Only the delta form survives the 32-bit
// millisecond wrap at ~49.7 days (TODO.md's `uptime` overflows after ~49.7
// days), the same shape elapsedMs below already relies on -- also flagged by
// Copilot's review, which is why it is spelled out here.
//
// sendHeartbeat and sendSystemTime are unconditional in every configuration,
// reduced included: this is the only task that runs in every configuration
// (moved here from TaskHeartbeat, review.md finding 16), so the stability-
// window clear and the 30-minute reduced-configuration retry below can only be
// trusted to fire if HEARTBEAT's entry is never made conditional.
// sendSystemTime is seeded half an interval (500 ms) behind sendHeartbeat so
// the two keep leaving 500 ms apart, exactly as when they alternated on one
// vTaskDelayUntil.
struct ScheduleEntry {
    void (*function)();
    uint32_t interval_ms;
    uint32_t last_ms;
    bool enabled;
};

[[noreturn]] void TaskMavlink(void *pvParameters)
{
    (void)pvParameters;

    sendBootStatusText();

    TickType_t bootTick = xTaskGetTickCount();
    uint32_t startMs = (uint32_t)bootTick * portTICK_PERIOD_MS;
    bool stabilityCleared = false;
    bool retryAttempted = false;

    // Housekeeping starts disabled and at the 1000 ms floor -- the interval
    // and enabled flag are both overwritten by MAV_CMD_SET_MESSAGE_INTERVAL
    // below; these are placeholder values for a disabled entry, not a rate
    // anything sends at. See specs/mavlink-link/spec.md, "off by default".
    ScheduleEntry schedule[] = {
        { sendHeartbeat,     1000, startMs - 1000u, true },
        { sendSystemTime,    1000, startMs - 500u,  true },
        { sendBatteryStatus, 2000, startMs - 2000u, !reducedConfiguration },
        { sendHousekeeping,  1000, startMs,         false },
    };
    constexpr uint8_t kHousekeepingScheduleIndex = 3;

    for (;;)
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        uint32_t waitMs = UINT32_MAX;
        for (const ScheduleEntry &entry : schedule) {
            if (!entry.enabled) continue;
            uint32_t elapsed = now - entry.last_ms;
            uint32_t due = (elapsed >= entry.interval_ms) ? 0 : (entry.interval_ms - elapsed);
            if (due < waitMs) waitMs = due;
        }

        mavlink_message_t* msg;
        if (xQueueReceive(serialReadQueue, &msg, pdMS_TO_TICKS(waitMs)))
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

                    if (command.command == MAV_CMD_SET_MESSAGE_INTERVAL
                        && (uint16_t)command.param1 == MAVLINK_MSG_ID_NAMED_VALUE_INT) {
                        // param2 is microseconds (matching MESSAGE_INTERVAL's own
                        // interval_us field), not milliseconds -- converted before
                        // it is compared against or stored in interval_ms
                        // (design.md's Context; an earlier version of this task
                        // stored it unconverted, which Copilot's review caught:
                        // a 1 Hz request would have run at 1000 seconds).
                        ScheduleEntry &housekeeping = schedule[kHousekeepingScheduleIndex];
                        int32_t requestedUs = (int32_t)command.param2;

                        if (requestedUs <= 0) {
                            // -1 disables. 0 asks for "the default rate", and
                            // that default is off -- both in the clean-boot
                            // case and stopping an already-armed stream
                            // (specs/mavlink-link/spec.md, "asks for the
                            // default rate").
                            housekeeping.enabled = false;
                            sendCommandAck(command.command, MAV_RESULT_ACCEPTED);
                        } else {
                            uint32_t requestedMs = (uint32_t)requestedUs / 1000u;
                            if (requestedMs < 1000u) {
                                // Below the floor -- denied, not silently
                                // clamped (design.md's "Converting the
                                // command's interval, and rejecting one that
                                // is too fast"). Publishing state is left
                                // exactly as it was.
                                sendCommandAck(command.command, MAV_RESULT_DENIED);
                            } else {
                                housekeeping.interval_ms = requestedMs;
                                housekeeping.last_ms = now;
                                housekeeping.enabled = true;
                                sendCommandAck(command.command, MAV_RESULT_ACCEPTED);
                            }
                        }
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

        now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (ScheduleEntry &entry : schedule) {
            if (entry.enabled && (now - entry.last_ms) >= entry.interval_ms) {
                entry.function();
                entry.last_ms += entry.interval_ms;
            }
        }

        // Both live here because this is the only task that runs in every
        // configuration -- reduced included -- so it is the only place that
        // can be trusted to ever clear the counter or fire the retry
        // (review.md finding 16, moved from TaskHeartbeat).
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