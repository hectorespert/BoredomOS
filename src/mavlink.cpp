#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Battery.h>
#include <SystemTime.h>
#include <Recovery.h>
#include <LinkMsg.h>
#include <LinkPort.h>
#include <Link.h>
#include <MavlinkPack.h>
#include <string.h>

// Both ports, owned by src/link.cpp and only read here. Every send* below
// takes the port index it is for: a reply goes out the port its request
// arrived on, and each port's periodic schedule is its own
// (specs/mavlink-link/spec.md, "Each port is an independent MAVLink stream").
//
// The index is the channel number: LINK_CHAN_UART is MAVLINK_COMM_0 and
// LINK_CHAN_USB is MAVLINK_COMM_1, so an InboundMsg's chan indexes linkPorts
// directly. The static_asserts below are what keep that true if either
// definition moves.
extern LinkPort linkPorts[2];

constexpr uint8_t kPortCount = 2;
static_assert(LINK_CHAN_UART == 0, "linkPorts is indexed by channel number");
static_assert(LINK_CHAN_USB == 1, "linkPorts is indexed by channel number");

extern SystemTime systemTime;

extern bool reducedConfiguration;
extern Recovery::ResetReason previousResetReason;
extern Recovery::BootPhase previousBootPhase;

// Set once in setup(), before the scheduler starts (src/main.cpp:335-357), and
// never reassigned after -- safe to read directly from the housekeeping table
// below rather than snapshotting them into a table built at static-init time,
// which would run before setup() and see every handle still NULL.
extern TaskHandle_t taskUartReadHandler;
extern TaskHandle_t taskUartWriteHandler;
extern TaskHandle_t taskUsbReadHandler;
extern TaskHandle_t taskUsbWriteHandler;
extern TaskHandle_t taskMavlinkHandler;
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

static void sendHeartbeat(uint8_t port) {
    LinkMsg intent;
    intent.kind = LinkMsgKind::Heartbeat;
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
}

static void sendSystemTime(uint8_t port)
{
    LinkMsg intent;
    intent.kind = LinkMsgKind::SystemTime;
    // Zero is the protocol's "not known", and what ArduPilot's send_system_time()
    // leaves in the field when its RTC cannot supply a time. With no origin the
    // internal RTC is counting from the epoch, so reporting its reading would
    // put a date in 1970 on the wire that a ground station cannot tell from a
    // clock genuinely set to 1970. time_boot_ms still carries the useful figure.
    intent.system_time.unix_usec =
        systemTime.source() == SystemTime::Source::None ? 0 : systemTime.getUnixTimeUsec();
    intent.system_time.boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
}

static void sendStatusText(uint8_t port, const char* text, uint8_t severity)
{
    LinkMsg intent;
    intent.kind = LinkMsgKind::StatusText;
    intent.statustext.severity = severity;
    strncpy(intent.statustext.text, text, sizeof(intent.statustext.text) - 1);
    intent.statustext.text[sizeof(intent.statustext.text) - 1] = '\0';
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
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
//
// Goes out both ports rather than one: it answers "why did you reset", and
// which port a ground station happens to be on at boot is not something the
// firmware knows. Unlike a reply, it has no originating port to route back to.
static void sendBootStatusText()
{
    char text[50];
    text[0] = '\0';
    strncat(text, "Reset: ", sizeof(text) - 1);
    strncat(text, resetReasonText(previousResetReason), sizeof(text) - 1 - strlen(text));
    strncat(text, ", phase ", sizeof(text) - 1 - strlen(text));
    strncat(text, bootPhaseText(previousBootPhase), sizeof(text) - 1 - strlen(text));

    for (uint8_t port = 0; port < kPortCount; ++port) {
        sendStatusText(port, text, MAV_SEVERITY_CRITICAL);
    }
}

static const char* clockSourceText(SystemTime::Source source)
{
    switch (source) {
        case SystemTime::Source::Ground: return "ground";
        case SystemTime::Source::Ds1307: return "ds1307";
        case SystemTime::Source::Survived: return "survived";
        case SystemTime::Source::None: return "none";
    }
    return "unknown";
}

// A second text rather than more of the one above: that one is already 48 of its
// 50 characters in its longest combination, so there is no room left in it.
//
// "found live" is reported separately from the origin, and is not derived from
// it, because ds1307 outranks survived -- on a board whose DS1307 cannot be
// disconnected the origin alone would never reveal that the internal RTC kept
// running across a reset. Longest combination here is 25 characters
// ("Clock: survived, found live").
static void sendClockStatusText()
{
    char text[50];
    text[0] = '\0';
    strncat(text, "Clock: ", sizeof(text) - 1);
    strncat(text, clockSourceText(systemTime.source()), sizeof(text) - 1 - strlen(text));
    strncat(text, systemTime.foundClockRunning() ? ", found live" : ", found none",
            sizeof(text) - 1 - strlen(text));

    for (uint8_t port = 0; port < kPortCount; ++port) {
        sendStatusText(port, text, MAV_SEVERITY_INFO);
    }
}

static void sendCommandAck(uint8_t port, uint16_t command, uint8_t result)
{
    LinkMsg intent;
    intent.kind = LinkMsgKind::CommandAck;
    intent.command_ack.command = command;
    intent.command_ack.result = result;
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
}

extern Battery battery;

static void sendBatteryStatus(uint8_t port)
{
    LinkMsg intent;
    intent.kind = LinkMsgKind::BatteryStatus;
    intent.battery.millivolts = battery.millivolts();
    intent.battery.remaining = battery.remaining();
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
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
// "Mavlink") would read past the end of it. Declaring these as char arrays
// makes the compiler pad the remainder with zeros instead.
//
// NAMED_VALUE_INT.name is char[10], and that -- not configMAX_TASK_NAME_LEN,
// which is 16 -- is the binding limit on a task name in this firmware. It is
// what used to truncate "SerialWrite" to "SerialWrit" on the wire. Every name
// below now fits in 10 with room to spare and none collides with another after
// truncation, which matters because telling per-task high-water marks apart is
// the only reason this stream exists (design.md, Decision 10).
//
// 12 bytes, not 10: a char[10] initialized from a 10-character literal (no
// room left for the implicit terminator, e.g. "SerialRead") is a hard error
// under this toolchain's g++ (-fpermissive would only downgrade the
// standard-permitted exact-length case to a warning, and this build does not
// pass that flag). 12 is comfortably more than the 10 bytes the pack
// function ever reads from these, for every name here.
static const char kNameHeapFree[12]  = "HeapFree";
static const char kNameHeapMin[12]   = "HeapMin";
static const char kNameUartRead[12]  = "UartRead";
static const char kNameUartWrite[12] = "UartWrite";
static const char kNameUsbRead[12]   = "UsbRead";
static const char kNameUsbWrite[12]  = "UsbWrite";
static const char kNameMavlink[12]   = "Mavlink";
static const char kNameLogger[12]    = "Logger";
static const char kNameSdWrite[12]   = "SdWrite";

struct HousekeepingTaskEntry {
    TaskHandle_t *handle;
    const char *name;
};

// Points at the six extern handles above rather than copying their values, so
// a NULL check always sees setup()'s actual outcome for this boot. Cycle
// length follows the task count -- a future task added to src/main.cpp needs
// an entry here too, and needs each port's write queue depth re-checked,
// per design.md's Risks ("The cycle length grows every time a task is
// added").
static const HousekeepingTaskEntry kHousekeepingTasks[] = {
    { &taskUartReadHandler,  kNameUartRead  },
    { &taskUartWriteHandler, kNameUartWrite },
    { &taskUsbReadHandler,   kNameUsbRead   },
    { &taskUsbWriteHandler,  kNameUsbWrite  },
    { &taskMavlinkHandler,   kNameMavlink   },
    { &taskLoggerHandler,    kNameLogger    },
    { &taskSdWriteHandler,   kNameSdWrite   },
};

constexpr uint8_t kHousekeepingHeapSlots = 2;
constexpr uint8_t kHousekeepingTaskSlots = sizeof(kHousekeepingTasks) / sizeof(kHousekeepingTasks[0]);
constexpr uint8_t kHousekeepingSlots = kHousekeepingHeapSlots + kHousekeepingTaskSlots;

// One cursor per port, not one shared. Arming is per port
// (specs/mavlink-link/spec.md, "Arming one port leaves the other alone"), so
// each port has to keep its own position in the cycle -- a shared cursor would
// make two armed ground stations each see every other value.
static uint8_t housekeepingCursor[kPortCount] = { 0, 0 };

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
static bool nextHousekeepingValue(uint8_t port, const char **name, int32_t *value)
{
    for (uint8_t tries = 0; tries < kHousekeepingSlots; ++tries) {
        uint8_t slot = housekeepingCursor[port];
        housekeepingCursor[port] = (housekeepingCursor[port] + 1) % kHousekeepingSlots;

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

// Follows sendHeartbeat/sendSystemTime's shape: fill the intent, xQueueSend
// onto that port's write queue, nothing to free either way. One value per
// call -- never a burst of several -- so this entry is structurally
// identical to every other one from the queue's point of view (design.md's
// "one ScheduleEntry function that sends all N values" was rejected for
// exactly this reason).
static void sendHousekeeping(uint8_t port)
{
    const char *name;
    int32_t value;
    if (!nextHousekeepingValue(port, &name, &value)) return;

    LinkMsg intent;
    intent.kind = LinkMsgKind::NamedValueInt;
    intent.named_value_int.name = name;
    intent.named_value_int.value = value;
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
}

// The only function in the firmware outside this file that may call a
// mavlink_msg_*_pack function -- src/link.cpp's TaskLinkWrite calls this to
// turn what a producer meant into a wire-ready message, keeping protocol
// knowledge here rather than in the transport (design.md's Decisions,
// queue-mavlink-messages-by-value).
//
// Every call below is the _pack_chan form, not the plain _pack one. The plain
// form takes its sequence number from channel 0 unconditionally, so with two
// ports both streams would share one counter and each ground station would see
// gaps and infer packet loss. current_tx_seq lives in mavlink_status_t[chan],
// so passing the channel is the whole of what makes the numbering per-port --
// see specs/mavlink-link/spec.md, "Each port is an independent MAVLink
// stream", and design.md, Decision 5.
void mavlinkPack(uint8_t chan, const LinkMsg &intent, mavlink_message_t *out)
{
    switch (intent.kind) {
        case LinkMsgKind::Heartbeat: {
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
            break;
        }

        case LinkMsgKind::SystemTime:
            mavlink_msg_system_time_pack_chan(
                1,
                MAV_COMP_ID_AUTOPILOT1,
                chan,
                out,
                intent.system_time.unix_usec,
                intent.system_time.boot_ms
            );
            break;

        case LinkMsgKind::BatteryStatus: {
            uint16_t voltages[10];
            voltages[0] = intent.battery.millivolts;
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
                intent.battery.remaining,
                0,
                MAV_BATTERY_CHARGE_STATE_UNDEFINED,
                voltages_ext,
                MAV_BATTERY_MODE_UNKNOWN,
                0
            );
            break;
        }

        case LinkMsgKind::TimesyncReply:
            mavlink_msg_timesync_pack_chan(
                1,
                MAV_COMP_ID_AUTOPILOT1,
                chan,
                out,
                intent.timesync.tc1,
                intent.timesync.ts1,
                intent.timesync.target_system,
                intent.timesync.target_component
            );
            break;

        case LinkMsgKind::StatusText:
            mavlink_msg_statustext_pack_chan(
                1,
                MAV_COMP_ID_AUTOPILOT1,
                chan,
                out,
                intent.statustext.severity,
                intent.statustext.text,
                0,
                0
            );
            break;

        case LinkMsgKind::CommandAck:
            mavlink_msg_command_ack_pack_chan(
                1,
                MAV_COMP_ID_AUTOPILOT1,
                chan,
                out,
                intent.command_ack.command,
                intent.command_ack.result,
                0,
                0,
                0,
                0
            );
            break;

        case LinkMsgKind::NamedValueInt: {
            uint32_t boot_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            mavlink_msg_named_value_int_pack_chan(
                1,
                MAV_COMP_ID_AUTOPILOT1,
                chan,
                out,
                boot_ms,
                intent.named_value_int.name,
                intent.named_value_int.value
            );
            break;
        }
    }
}

extern QueueHandle_t linkReadQueue;
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
//
// One schedule per port, not one shared. Each port is an independent stream,
// so each keeps its own last_ms and its own housekeeping arming -- a shared
// table would make arming one port arm both and the cadences interlock
// (design.md, Decision 2; specs/mavlink-link/spec.md).
struct ScheduleEntry {
    void (*function)(uint8_t port);
    uint32_t interval_ms;
    uint32_t last_ms;
    bool enabled;
};

[[noreturn]] void TaskMavlink(void *pvParameters)
{
    (void)pvParameters;

    sendBootStatusText();
    sendClockStatusText();

    TickType_t bootTick = xTaskGetTickCount();
    uint32_t startMs = (uint32_t)bootTick * portTICK_PERIOD_MS;
    bool stabilityCleared = false;
    bool retryAttempted = false;

    // Housekeeping starts disabled and at the 1000 ms floor -- the interval
    // and enabled flag are both overwritten by MAV_CMD_SET_MESSAGE_INTERVAL
    // below; these are placeholder values for a disabled entry, not a rate
    // anything sends at. See specs/mavlink-link/spec.md, "off by default".
    ScheduleEntry schedule[kPortCount][4] = {
        {
            { sendHeartbeat,     1000, startMs - 1000u, true },
            { sendSystemTime,    1000, startMs - 500u,  true },
            { sendBatteryStatus, 2000, startMs - 2000u, !reducedConfiguration },
            { sendHousekeeping,  1000, startMs,         false },
        },
        {
            { sendHeartbeat,     1000, startMs - 1000u, true },
            { sendSystemTime,    1000, startMs - 500u,  true },
            { sendBatteryStatus, 2000, startMs - 2000u, !reducedConfiguration },
            { sendHousekeeping,  1000, startMs,         false },
        },
    };
    constexpr uint8_t kHousekeepingScheduleIndex = 3;

    // Deliberately NOT an entry in the schedule above. That table is per port and
    // every entry in it emits a message; this emits nothing and is not per port,
    // so putting it there would imply a fifth independently-clocked producer and
    // invalidate the write-queue depth justification in src/main.cpp beside
    // uartWriteQueue. Same unsigned-delta comparison, for the same wrap reason.
    //
    // PROVISIONAL interval: task 1.3 of this change measures how far the
    // LOCO-driven internal RTC actually drifts from the DS1307 and settles this
    // number. Six hours is a conservative placeholder, not a measured value.
    constexpr uint32_t kClockReseedIntervalMs = 6u * 60u * 60u * 1000u;
    uint32_t lastReseedMs = startMs;

    for (;;)
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Keeps the two clocks in agreement so they do not drift apart for a
        // whole mission with nobody reconciling them. The direction follows the
        // ladder: with a ground-set clock the internal one is the authority and is
        // written out to the DS1307, so the next boot seeds from something current;
        // otherwise the DS1307 is the better keeper and seeds the internal one.
        //
        // This is the only place that job happens. Doing it on every inbound
        // SYSTEM_TIME instead made the reference GCS's 1 Hz clock updates cost an
        // I2C round trip a second -- Copilot's review of this change.
        if ((now - lastReseedMs) >= kClockReseedIntervalMs) {
            lastReseedMs += kClockReseedIntervalMs;
            SystemTime::Source beforeReconcile = systemTime.source();

            bool reconciled = (beforeReconcile == SystemTime::Source::Ground)
                                  ? systemTime.pushToDs1307()
                                  : systemTime.reseedFromDs1307();

            // Reports a change like any other: a re-seed can promote `survived` to
            // `ds1307`, and the ground has no other way to learn that the clock it
            // is reading now has a different provenance. Bounded by the interval,
            // not by a peer, so it cannot flood.
            if (reconciled && systemTime.source() != beforeReconcile) {
                sendClockStatusText();
            }
        }

        uint32_t waitMs = UINT32_MAX;
        for (uint8_t p = 0; p < kPortCount; ++p) {
            for (const ScheduleEntry &entry : schedule[p]) {
                if (!entry.enabled) continue;
                uint32_t elapsed = now - entry.last_ms;
                uint32_t due = (elapsed >= entry.interval_ms) ? 0 : (entry.interval_ms - elapsed);
                if (due < waitMs) waitMs = due;
            }
        }

        // The item carries the port it arrived on. Every reply below goes to
        // `port` and nowhere else, which is what makes a request answered on
        // the link that asked it rather than broadcast to both
        // (specs/mavlink-link/spec.md, "A command arrives on one port").
        InboundMsg inbound;
        if (xQueueReceive(linkReadQueue, &inbound, pdMS_TO_TICKS(waitMs)))
        {
            const mavlink_message_t &msg = inbound.msg;
            const uint8_t port = inbound.chan;

            switch (msg.msgid)
            {
                case MAVLINK_MSG_ID_HEARTBEAT:
                    break;

                case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
                    break;

                case MAVLINK_MSG_ID_COMMAND_LONG:
                    mavlink_command_long_t command;
                    mavlink_msg_command_long_decode(&msg, &command);

                    if (command.command == MAV_CMD_GET_HOME_POSITION) {
                        break;
                    }

                    if (command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) {
                        // Acknowledged rather than silently handled -- review.md
                        // finding 17. This is a new message on the wire; the
                        // closed-set HIL case (test/test_hil/check_recovery.py,
                        // task 7.5) must expect it.
                        sendCommandAck(port, command.command, MAV_RESULT_ACCEPTED);

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
                        ScheduleEntry &housekeeping = schedule[port][kHousekeepingScheduleIndex];
                        int32_t requestedUs = (int32_t)command.param2;

                        if (requestedUs <= 0) {
                            // -1 disables. 0 asks for "the default rate", and
                            // that default is off -- both in the clean-boot
                            // case and stopping an already-armed stream
                            // (specs/mavlink-link/spec.md, "asks for the
                            // default rate").
                            housekeeping.enabled = false;
                            sendCommandAck(port, command.command, MAV_RESULT_ACCEPTED);
                        } else {
                            uint32_t requestedMs = (uint32_t)requestedUs / 1000u;
                            if (requestedMs < 1000u) {
                                // Below the floor -- denied, not silently
                                // clamped (design.md's "Converting the
                                // command's interval, and rejecting one that
                                // is too fast"). Publishing state is left
                                // exactly as it was.
                                sendCommandAck(port, command.command, MAV_RESULT_DENIED);
                            } else {
                                housekeeping.interval_ms = requestedMs;
                                housekeeping.last_ms = now;
                                housekeeping.enabled = true;
                                sendCommandAck(port, command.command, MAV_RESULT_ACCEPTED);
                            }
                        }
                    }

                    break;

                case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
                    break;

                case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:
                    break;

                case MAVLINK_MSG_ID_SYSTEM_TIME: {
                    time_t unix_time_from_gcs = mavlink_msg_system_time_get_time_unix_usec(&msg) / USEC_PER_SEC;
                    SystemTime::Source before = systemTime.source();
                    // Nothing is emitted when this is refused. The sender decides
                    // how often it offers a time, so a text per rejection would
                    // let a peer generate unbounded outbound traffic and starve
                    // the periodic cadences specs/mavlink-link/spec.md
                    // guarantees. A refusal is observable anyway: the reported
                    // time does not move.
                    if (systemTime.setUnixTime(unix_time_from_gcs, SystemTime::Source::Ground)
                        && systemTime.source() != before) {
                        sendClockStatusText();
                    }
                    break;
                }

                case MAVLINK_MSG_ID_TIMESYNC: {
                    mavlink_timesync_t timesync;
                    mavlink_msg_timesync_decode(&msg, &timesync);

                    if (timesync.tc1 == 0) {
                        LinkMsg intent;
                        intent.kind = LinkMsgKind::TimesyncReply;
                        // Captured here, on receipt, not in mavlinkPack() after
                        // the queue hop -- see include/LinkMsg.h.
                        intent.timesync.tc1 = systemTime.sinceBootNsec();
                        intent.timesync.ts1 = timesync.ts1;
                        intent.timesync.target_system = timesync.target_system;
                        intent.timesync.target_component = timesync.target_component;
                        xQueueSend(linkPorts[port].writeQueue, &intent, 0);
                    }

                    break;
                }

                default:
                    // Pre-existing defect fixed incidentally while converting
                    // this call site (queue-mavlink-messages-by-value, task
                    // 2.3): the previous text did pointer arithmetic on a
                    // string literal ("..." + msg->msgid) instead of
                    // concatenation, producing a truncated or out-of-bounds
                    // substring for any msgid at or past the literal's
                    // length. A fixed string avoids that without adding a
                    // formatting helper this path does not otherwise need.
                    sendStatusText(port, "Unhandled message received", MAV_SEVERITY_WARNING);
                    break;
            }
        }

        now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (uint8_t p = 0; p < kPortCount; ++p) {
            for (ScheduleEntry &entry : schedule[p]) {
                if (entry.enabled && (now - entry.last_ms) >= entry.interval_ms) {
                    entry.function(p);
                    entry.last_ms += entry.interval_ms;
                }
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