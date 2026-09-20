#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SdRecord.h>
#include <SystemTime.h>

extern SystemTime systemTime;

extern QueueHandle_t sdWriteQueue;

extern TaskHandle_t taskLoggerHandler;
extern TaskHandle_t taskSdWriteHandler;
extern TaskHandle_t taskMavlinkHandler;
extern TaskHandle_t taskUartWriteHandler;
extern TaskHandle_t taskUartReadHandler;
extern TaskHandle_t taskUsbWriteHandler;
extern TaskHandle_t taskUsbReadHandler;

// The log exists to size the stacks, and this is the task that samples them.
//
// It does NOT read the battery: that record comes from TaskMavlink, which already
// reads lib/Battery on its own schedule. Reading it from here as well would make
// Battery's unguarded 125 ms cache state shared between two tasks of different
// priorities, which is the rule that keeps this firmware free of mutexes.
[[noreturn]] void TaskLogger(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        SdRecord record;
        record.kind = SdRecordKind::Sys;

        // Elapsed time from lib/SystemTime, as a subtraction against the boot epoch.
        // Nothing accumulates and nothing wraps, which is what retires the old
        // uptime field's ~49.7-day overflow: that was xTaskGetTickCount() times the
        // tick period in a uint32_t. The resolution becomes the RTC's 1/128 s rather
        // than the tick's 1 ms, which is immaterial at 1 Hz.
        record.sys.timeUs = systemTime.sinceBootUsec();
        record.sys.heapFree = (uint16_t)xPortGetFreeHeapSize();

        // In the order SYS's labels declare them: Log, SdW, Mav, SRd, SWr, URd, UWr.
        record.sys.stacks[0] = (uint16_t)uxTaskGetStackHighWaterMark(taskLoggerHandler);
        record.sys.stacks[1] = (uint16_t)uxTaskGetStackHighWaterMark(taskSdWriteHandler);
        record.sys.stacks[2] = (uint16_t)uxTaskGetStackHighWaterMark(taskMavlinkHandler);
        record.sys.stacks[3] = (uint16_t)uxTaskGetStackHighWaterMark(taskUartReadHandler);
        record.sys.stacks[4] = (uint16_t)uxTaskGetStackHighWaterMark(taskUartWriteHandler);
        record.sys.stacks[5] = (uint16_t)uxTaskGetStackHighWaterMark(taskUsbReadHandler);
        record.sys.stacks[6] = (uint16_t)uxTaskGetStackHighWaterMark(taskUsbWriteHandler);

        // By value: nothing to allocate, so nothing to free on a refused send. A
        // record the queue will not take is one housekeeping sample lost, silently,
        // exactly as before -- see the backlog entry on SD logging failure.
        xQueueSend(sdWriteQueue, &record, 0);

        vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    }

}
