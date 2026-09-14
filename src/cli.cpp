#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Cli.h>
#include <MavlinkShared.h>
#include <string.h>

// The CLI owns CLI_SERIAL: it is the port's only writer while tasks run (the one
// documented exception is src/hooks.cpp's stack-overflow hook, which runs after
// the scheduler has stopped). No queue, no mutex: nothing else touches this port.
//
// Read-only by design: no command here may change firmware state. See
// specs/console-cli/spec.md -- "The CLI is read-only".

// One TaskStatus_t snapshot, static rather than on the stack (432 B, more than
// this task's 128-word stack) or the FreeRTOS heap (it would compete with
// queued messages for the 6 KB the tasks share). Sized for 12: 9 tasks exist
// today (8 ours, idle), see design.md -- "The snapshot buffer is static...".
static constexpr UBaseType_t kMaxTasks = 12;
static TaskStatus_t cliTaskStatus[kMaxTasks];

static constexpr size_t kLineBufSize = 32;
static char lineBuf[kLineBufSize];
static size_t lineLen = 0;
static bool lineOverflowed = false;

// Writes through CLI_SERIAL without ever spinning on a full transmit buffer.
// Print::write() on this core (both _SerialUSB and UART) busy-loops with no
// RTOS yield when there is no room to write into -- confirmed against
// cores/arduino/usb/SerialUSB.cpp's write(), which retries
// tud_cdc_write_available() in a bare while loop. Combined with
// configUSE_TIME_SLICING=0 and TaskCli sharing PRIORITY_LOWEST with
// TaskSdWrite and the idle task, a host that stops draining the port could
// starve both: the idle hook is what refreshes the watchdog, so this is not
// just a stalled reply, it is a path to a watchdog reset. Design.md's original
// "at PRIORITY_LOWEST that starves nothing" was wrong about this (Copilot
// review, src/cli.cpp:29/182, src/main.cpp:349) -- the fix is a write that
// yields instead of spins, not a higher priority.
static void writeChunk(const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t space = CLI_SERIAL.availableForWrite();
        if (space == 0) {
            vTaskDelay(1);
            continue;
        }
        size_t chunk = len - sent;
        if (chunk > space) chunk = space;
        size_t n = CLI_SERIAL.write(reinterpret_cast<const uint8_t *>(buf + sent), chunk);
        if (n == 0) {
            vTaskDelay(1);
            continue;
        }
        sent += n;
    }
}

static void writeText(const char *s)
{
    writeChunk(s, strlen(s));
}

static void writeChar(char c)
{
    writeChunk(&c, 1);
}

static void writeLine(const char *s)
{
    writeText(s);
    writeChunk("\r\n", 2);
}

static uint8_t decimalDigits(uint32_t value)
{
    uint8_t digits = 1;
    while (value >= 10) { value /= 10; digits++; }
    return digits;
}

static void writeDecimal(uint32_t value)
{
    char digits[10]; // uint32_t is at most 10 decimal digits
    uint8_t n = decimalDigits(value);
    uint32_t remaining = value;
    for (uint8_t i = n; i > 0; i--) {
        digits[i - 1] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    }
    writeChunk(digits, n);
}

// Formatting without printf: this core's Print has none (design.md -- "Formatting
// without printf"). Columns are aligned with manual padding, and every byte goes
// through writeChunk() rather than Print::write() -- see its comment above.
static void writeFieldPadded(const char *s, uint8_t width)
{
    writeText(s);
    size_t len = strlen(s);
    for (size_t i = len; i < width; i++) writeChar(' ');
}

static void writeFieldPaddedU(uint32_t value, uint8_t width)
{
    writeDecimal(value);
    uint8_t len = decimalDigits(value);
    for (uint8_t i = len; i < width; i++) writeChar(' ');
}

static char taskStateChar(eTaskState state)
{
    // The convention FreeRTOS's own vTaskList() uses for the same enum.
    switch (state) {
        case eRunning: return 'X';
        case eReady: return 'R';
        case eBlocked: return 'B';
        case eSuspended: return 'S';
        case eDeleted: return 'D';
        default: return '?';
    }
}

static void printPsHeader()
{
    writeFieldPadded("ID", 4);
    writeFieldPadded("NAME", 17);
    writeFieldPadded("PRI", 4);
    writeFieldPadded("ST", 3);
    writeLine("STACK");
}

static void printPsRow(const TaskStatus_t &task)
{
    writeFieldPaddedU(task.xTaskNumber, 4);
    writeFieldPadded(task.pcTaskName, 17);
    writeFieldPaddedU(task.uxCurrentPriority, 4);
    writeChar(taskStateChar(task.eCurrentState));
    writeText("  ");
    writeDecimal(task.usStackHighWaterMark);
    writeChunk("\r\n", 2);
}

// nameArg == NULL lists every task the kernel reports, ending with free heap.
// A non-NULL nameArg lists that one task only, with no heap line -- see
// specs/console-cli/spec.md, "ps accepts a task name to report a single task".
static void cmdPs(const char *nameArg)
{
    UBaseType_t count = uxTaskGetSystemState(cliTaskStatus, kMaxTasks, NULL);
    if (count == 0) {
        // uxTaskGetSystemState() returns 0 when the array cannot hold every
        // task -- report that as an explicit error, not an empty table.
        writeLine("error: too many tasks for the ps buffer");
        return;
    }

    if (nameArg == NULL) {
        printPsHeader();
        for (UBaseType_t i = 0; i < count; i++) {
            printPsRow(cliTaskStatus[i]);
        }
        writeText("heap free: ");
        writeDecimal(xPortGetFreeHeapSize());
        writeChunk("\r\n", 2);
        return;
    }

    // The kernel stores at most configMAX_TASK_NAME_LEN - 1 characters of a
    // name, so a longer argument is truncated the same way before comparing --
    // otherwise a full, untruncated name could never match.
    char truncatedName[configMAX_TASK_NAME_LEN];
    strncpy(truncatedName, nameArg, configMAX_TASK_NAME_LEN - 1);
    truncatedName[configMAX_TASK_NAME_LEN - 1] = '\0';

    for (UBaseType_t i = 0; i < count; i++) {
        if (strcmp(cliTaskStatus[i].pcTaskName, truncatedName) == 0) {
            printPsHeader();
            printPsRow(cliTaskStatus[i]);
            return;
        }
    }
    writeLine("error: no such task");
}

static void cmdFree()
{
    writeText("heap total: ");
    writeDecimal(configTOTAL_HEAP_SIZE);
    writeChunk("\r\n", 2);
    writeText("heap free: ");
    writeDecimal(xPortGetFreeHeapSize());
    writeChunk("\r\n", 2);
    writeText("heap free min: ");
    writeDecimal(xPortGetMinimumEverFreeHeapSize());
    writeChunk("\r\n", 2);
}

static void cmdHelp()
{
    writeLine("ps          - list every task known to the scheduler");
    writeLine("ps <name>   - list one task by name");
    writeLine("free        - heap total, free and minimum ever free");
    writeLine("help, ?     - this list");
}

static void printPrompt()
{
    writeText("> ");
}

// line is mutable: dispatch splits it in place at the first space rather than
// copying. An empty line does nothing here -- the caller's newline handling is
// what prints the bare prompt the spec's "An empty line" scenario asks for.
static void dispatch(char *line)
{
    if (line[0] == '\0') {
        return;
    }

    char *space = strchr(line, ' ');
    char *arg = NULL;
    if (space != NULL) {
        *space = '\0';
        arg = space + 1;
        while (*arg == ' ') arg++;
        if (*arg == '\0') arg = NULL;
    }

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        cmdHelp();
    } else if (strcmp(line, "free") == 0) {
        cmdFree();
    } else if (strcmp(line, "ps") == 0) {
        cmdPs(arg);
    } else {
        writeLine("error: unknown command, try 'help'");
    }
}

// Bounds how many input bytes one outer-loop pass consumes before falling
// through to vTaskDelay(10) below. Without this, a host streaming input
// continuously (available() staying nonzero) could keep TaskCli running
// indefinitely at PRIORITY_LOWEST -- the same starvation risk writeChunk()
// fixes on the output side, just from the input side instead (Copilot review,
// src/cli.cpp:184). 64 is well under what one 10 ms poll interval can receive
// at 115200 baud (~115 B), so a normal command still completes in one pass.
static constexpr uint8_t kMaxBytesPerPass = 64;

// ---------------------------------------------------------------------------
// Mode detection (design.md, "Switch on a checksum-valid frame, never on a
// header byte" and "The switch is one-way"). Every inbound byte is offered to
// this parser before the CLI-mode branch below ever sees it, mirroring
// madflight's Cli::update_MODE_CLI: a stray 0xFD from a terminal or a paste
// only starts a frame attempt, it does not switch the mode. Only a complete,
// checksum-valid frame does, and once it has, mavlinkMode never clears again
// short of a reset (task 3.3) -- see design.md's "one-way switch" decision
// for why no escape sequence or timeout exists.
//
// MAVLINK_COMM_1, not _0: src/serial.cpp's radio link owns _0 already, and a
// shared channel would mean a shared sequence counter -- see
// MavlinkShared.h's note on mavlink_finalize_message() hardcoding _0.
// ---------------------------------------------------------------------------

// One shared buffer for both directions: mavlink_parse_char() writes a
// completed inbound frame here, and mavlinkHandleInbound() then builds its
// reply into the *same* object -- msg and reply aliasing the same
// mavlink_message_t is safe only because every case in that function fully
// decodes what it needs from msg into locals (mavlink_command_long_t,
// mavlink_timesync_t, or a single scalar read) before ever writing to reply;
// see its definition in src/mavlink.cpp for the invariant this relies on.
// Scheduled telemetry reuses it the same way, one frame at a time, never
// concurrently -- TaskCli builds and sends a single frame before starting
// the next. Kept as one 291-byte static rather than two (or a 291-byte stack
// local in every caller): board-measured (task 4.6), the real MAVLink-mode
// stack peak left only 5 of 224 words free even after every affordable byte
// of stack growth and rebalancing src/main.cpp's over-provisioned
// mavlinkStack -- taking a second 291-byte item off the RAM budget entirely
// is what actually closed the gap. See design.md's "Stack growth" risk entry.
static mavlink_message_t mavlinkMsg;
static bool mavlinkMode = false;

// Wire size ceiling for what this task ever actually sends -- not
// MAVLINK_MAX_PACKET_LEN (280 B), which sizes for the protocol's absolute
// worst case (a 255-byte payload plus a signature block this firmware never
// uses). The four builders here top out at STATUSTEXT's 54-byte payload
// (MAVLINK_MSG_ID_STATUSTEXT_LEN); 80 covers that plus header, checksum and
// margin for a future message without chasing an exact-fit number. A buffer
// this size is safe only because every message on this path comes from our
// own four builders -- never a forwarded or attacker-sized payload.
static constexpr size_t kMaxOutFrameLen = 80;

// Packs msg and writes it only if the port already has room for the whole
// frame -- no allocation, no queue, no wait. Follows madflight's telem_send:
// a host that stops draining loses this frame rather than stalling the
// round-robin pass behind it. This is deliberately not writeChunk(), which
// retries until it can send everything -- that retry is what CLI-mode replies
// want (design.md accepts TaskCli parking on a stalled host) and exactly what
// the USB transmit path must not do (design.md, "The USB transmit path does
// not use the queue" -- lossy by design, see specs/mavlink-link/spec.md).
static bool sendMavlinkNonBlocking(const mavlink_message_t *msg)
{
    // Checked against msg->len -- the payload length -- before touching the
    // buffer, not after: mavlink_msg_to_send_buffer() writes based on this
    // same length with no bound of its own, so validating afterward would be
    // validating a write that already happened. 12 is MAVLink2's non-payload
    // overhead (10-byte header + 2-byte checksum; this firmware never signs).
    if ((size_t) msg->len + 12 > kMaxOutFrameLen) {
        return false;
    }

    uint8_t buf[kMaxOutFrameLen];
    uint16_t len = mavlink_msg_to_send_buffer(buf, msg);
    if (CLI_SERIAL.availableForWrite() < len) {
        return false;
    }
    CLI_SERIAL.write(buf, len);
    return true;
}

// The {function, interval, last} schedule (design.md, "The schedule lives in
// the console task, not in the existing producers"). One entry is attempted
// per pass, in round-robin order, stopping at the first entry that is due --
// whether or not its send succeeds -- so a full transmit buffer costs one
// failed attempt this pass, not a spin, and no single message can starve the
// others by always winning the race to be "due" first.
struct TelemetryEntry {
    void (*build)(mavlink_message_t *, uint8_t chan);
    uint32_t intervalMs;
    TickType_t lastTick;
};

static TelemetryEntry telemetrySchedule[] = {
    { mavlinkBuildHeartbeat, 1000, 0 },
    { mavlinkBuildSystemTime, 1000, 0 },
    { mavlinkBuildBatteryStatus, 2000, 0 },
};
static constexpr size_t kTelemetryCount = sizeof(telemetrySchedule) / sizeof(telemetrySchedule[0]);
static size_t telemetryCursor = 0;

static void runTelemetryPass()
{
    TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < kTelemetryCount; i++) {
        size_t idx = (telemetryCursor + i) % kTelemetryCount;
        TelemetryEntry &entry = telemetrySchedule[idx];
        uint32_t elapsedMs = (now - entry.lastTick) * portTICK_PERIOD_MS;

        if (elapsedMs >= entry.intervalMs) {
            entry.build(&mavlinkMsg, MAVLINK_COMM_1);
            if (sendMavlinkNonBlocking(&mavlinkMsg)) {
                entry.lastTick = now;
            }
            // Only the entry lastTick reflects skips a failed send -- moving
            // on regardless is what "one message per pass" means: this pass
            // is spent either way, not retried immediately for the same slot.
            telemetryCursor = (idx + 1) % kTelemetryCount;
            return;
        }
    }
}

// A reboot request's reply must reach the host before NVIC_SystemReset() --
// Serial1's caller in src/mavlink.cpp delays to let TaskSerialWrite drain a
// queue; here there is no queue, but the USB hardware still needs a moment to
// actually put the written bytes on the wire, not merely to have accepted
// them into its own FIFO. Same delay, same reasoning, different reason for
// needing it -- see MavlinkShared.h's note.
static void handleMavlinkFrame()
{
    bool rebootRequested = false;
    bool hasReply = mavlinkHandleInbound(&mavlinkMsg, MAVLINK_COMM_1, &mavlinkMsg, &rebootRequested);

    if (hasReply) {
        sendMavlinkNonBlocking(&mavlinkMsg);
    }

    if (rebootRequested) {
        vTaskDelay(pdMS_TO_TICKS(50));
        NVIC_SystemReset();
    }
}

// No unsolicited output in CLI mode: the console stays silent until the first
// byte arrives. Once mavlinkMode switches, the scheduled telemetry above is
// unsolicited by design -- that is the whole point of section 4 -- so this
// silence guarantee is scoped to CLI mode only (specs/console-cli/spec.md,
// "Firmware running normally").
[[noreturn]] void TaskCli(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        uint8_t processed = 0;
        while (processed < kMaxBytesPerPass && CLI_SERIAL.available() > 0)
        {
            char c = (char) CLI_SERIAL.read();
            processed++;

            uint8_t frameComplete = mavlink_parse_char(MAVLINK_COMM_1, (uint8_t) c, &mavlinkMsg, NULL);

            if (frameComplete) {
                mavlinkMode = true; // one-way; re-setting true is harmless
                handleMavlinkFrame();
                continue;
            }

            if (mavlinkMode) {
                // Mid-frame bytes or parse noise while already switched:
                // never fed to the command buffer once in MAVLink mode
                // (task 3.4) -- the CLI is unreachable here by design.
                continue;
            }

            // Still in CLI mode, and this byte did not complete a MAVLink
            // frame: treat it as command text. Line noise or a partial frame
            // that never completes lands here too and is handled the same as
            // any other unrecognised text (design.md's "Line noise on an
            // open port" scenario) -- nothing about mode detection needs the
            // CLI's own framing to be noise-aware.
            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                if (lineOverflowed) {
                    writeLine("error: line too long");
                } else {
                    lineBuf[lineLen] = '\0';
                    dispatch(lineBuf);
                }
                lineLen = 0;
                lineOverflowed = false;
                printPrompt();
                continue;
            }

            if (lineLen < kLineBufSize - 1) {
                lineBuf[lineLen++] = c;
            } else {
                // Overlong line: keep discarding bytes until the newline
                // rather than overflowing the buffer.
                lineOverflowed = true;
            }
        }

        // Task 4.3: pending input is fully drained above before telemetry is
        // even considered, so a reply is never starved by the schedule below.
        if (mavlinkMode) {
            runTelemetryPass();
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
