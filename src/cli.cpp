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

// One TaskStatus_t snapshot, static rather than on the stack (432 B would not fit
// a 192-word stack) or the FreeRTOS heap (it would compete with queued messages
// for the 8 KB the tasks share). Sized for 12: 9 tasks exist today (7 ours, idle,
// timer service), see design.md -- "The snapshot buffer is static...".
static constexpr UBaseType_t kMaxTasks = 12;
static TaskStatus_t cliTaskStatus[kMaxTasks];

static constexpr size_t kLineBufSize = 32;
static char lineBuf[kLineBufSize];
static size_t lineLen = 0;
static bool lineOverflowed = false;

// Formatting without printf: this core's Print has none (design.md -- "Formatting
// without printf"). Columns are aligned with print() and a padding loop instead.
static void printField(const char *s, uint8_t width)
{
    CLI_SERIAL.print(s);
    size_t len = strlen(s);
    for (size_t i = len; i < width; i++) CLI_SERIAL.print(' ');
}

static uint8_t decimalDigits(uint32_t value)
{
    uint8_t digits = 1;
    while (value >= 10) { value /= 10; digits++; }
    return digits;
}

static void printFieldU(uint32_t value, uint8_t width)
{
    CLI_SERIAL.print(value);
    uint8_t len = decimalDigits(value);
    for (uint8_t i = len; i < width; i++) CLI_SERIAL.print(' ');
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
    printField("ID", 4);
    printField("NAME", 17);
    printField("PRI", 4);
    printField("ST", 3);
    CLI_SERIAL.println("STACK");
}

static void printPsRow(const TaskStatus_t &task)
{
    printFieldU(task.xTaskNumber, 4);
    printField(task.pcTaskName, 17);
    printFieldU(task.uxCurrentPriority, 4);
    CLI_SERIAL.print(taskStateChar(task.eCurrentState));
    CLI_SERIAL.print("  ");
    CLI_SERIAL.println(task.usStackHighWaterMark);
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
        CLI_SERIAL.println("error: too many tasks for the ps buffer");
        return;
    }

    if (nameArg == NULL) {
        printPsHeader();
        for (UBaseType_t i = 0; i < count; i++) {
            printPsRow(cliTaskStatus[i]);
        }
        CLI_SERIAL.print("heap free: ");
        CLI_SERIAL.println(xPortGetFreeHeapSize());
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
    CLI_SERIAL.println("error: no such task");
}

static void cmdFree()
{
    CLI_SERIAL.print("heap total: ");
    CLI_SERIAL.println(configTOTAL_HEAP_SIZE);
    CLI_SERIAL.print("heap free: ");
    CLI_SERIAL.println(xPortGetFreeHeapSize());
    CLI_SERIAL.print("heap free min: ");
    CLI_SERIAL.println(xPortGetMinimumEverFreeHeapSize());
}

static void cmdHelp()
{
    CLI_SERIAL.println("ps          - list every task known to the scheduler");
    CLI_SERIAL.println("ps <name>   - list one task by name");
    CLI_SERIAL.println("free        - heap total, free and minimum ever free");
    CLI_SERIAL.println("help, ?     - this list");
}

static void printPrompt()
{
    CLI_SERIAL.print("> ");
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
        CLI_SERIAL.println("error: unknown command, try 'help'");
    }
}

// No unsolicited output: the console stays silent until the first byte arrives,
// so check_silence.py's "nothing on the USB console" still holds for a build
// that has not moved the CLI elsewhere (design.md, "A consequence for the
// existing suite").
[[noreturn]] void TaskCli(void *pvParameters)
{
    (void) pvParameters;

    for (;;)
    {
        while (CLI_SERIAL.available() > 0)
        {
            char c = (char) CLI_SERIAL.read();

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                if (lineOverflowed) {
                    CLI_SERIAL.println("error: line too long");
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
