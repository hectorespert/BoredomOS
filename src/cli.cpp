#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Cli.h>
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

// No unsolicited output: the console stays silent until the first byte arrives,
// so check_silence.py's "nothing on the USB console" still holds for a build
// that has not moved the CLI elsewhere (design.md, "A consequence for the
// existing suite").
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

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
