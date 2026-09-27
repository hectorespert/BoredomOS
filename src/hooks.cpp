#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Recovery.h>
#include <boot.h>
#include "r_wdt.h"

// The idle task's memory. With configSUPPORT_STATIC_ALLOCATION the kernel does not
// allocate it: it asks the application for storage it can use, once, before the
// scheduler starts. configUSE_TIMERS is 0, so the matching timer-service callback is
// never linked and is deliberately absent.
static StaticTask_t idleTaskBuffer;
static StackType_t idleTaskStack[configMINIMAL_STACK_SIZE];

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                              StackType_t **ppxIdleTaskStackBuffer,
                                              uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &idleTaskBuffer;
    *ppxIdleTaskStackBuffer = idleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

extern wdt_instance_ctrl_t wdtCtrl;

// Declared but not exported by any header: portable/FSP/port.c defines it with
// external linkage and its own weak vApplicationIdleHook() calls it directly.
// Overriding that hook means reproducing this declaration too.
extern "C" void rm_freertos_port_sleep_preserving_lpm(uint32_t xExpectedIdleTime);

// Overrides the port's weak vApplicationIdleHook() (portable/FSP/port.c). Runs
// only when every other task is blocked or delayed, so a task that stops
// yielding stops this from running at all -- see design.md.
//
// The DFU-upload flag is tested first, and jumped on directly instead of
// refreshing: relying on the watchdog to underflow during the core's DFU
// callback cannot work (review.md finding 1 -- dfu-util's 1000 ms detach
// timeout cannot fit a watchdog period long enough for TaskSdWrite's worst
// case). goBootloader() rewrites the double-tap magic and calls
// NVIC_SystemReset() directly, which is what the core's own weak hook already
// expects: it skips the sleep below whenever this flag is set
// (port.c:1482-1484), on the assumption that something is actively trying to
// reach the bootloader.
//
// The rest of this function reproduces the port's own sleep sequence exactly.
// Dropping it would cost low-power idle permanently, which matters on a
// solar-powered satellite.
extern "C" void vApplicationIdleHook(void)
{
    __disable_irq();

    vTaskSuspendAll();

    extern bool is_watchdog_reset_in_progress_for_upload;
    if (is_watchdog_reset_in_progress_for_upload) {
        goBootloader();
    }

    R_WDT_Refresh(&wdtCtrl);
    rm_freertos_port_sleep_preserving_lpm(1);

    __enable_irq();

    (void) xTaskResumeAll();
}

// Both fault hooks below do the same three things and nothing else: mask interrupts,
// record which fault and in which task, and reset. Each runs when the firmware can no
// longer be trusted -- next to a stack that has just overrun its neighbours, or with
// memory exhausted -- so neither touches the serial port, the LED, the tick or an
// allocation. The record is read back at the next boot, which reports it on the link
// and in the log (report-the-faulting-task). LED_BUILTIN is the SD card's SPI clock
// besides, so no indication on it could outlast the reset anyway.
//
// NVIC_SystemReset() rather than letting the watchdog underflow: the reset then
// happens at once instead of up to one watchdog period later, and it is recorded as a
// software reset with no deliberate-reset marker, which advances the consecutive fault
// count just as the watchdog reset used to. The phase byte is what tells it apart.

// Called from the context switch, in the PendSV handler on the main stack, not on the
// task stack that overflowed. pcTaskName points into that task's TCB, which a long
// enough overrun could have damaged; Recovery::recordFault bounds the copy, and every
// reader filters what it reads back.
// The signature is FreeRTOS's (task.h declares pcTaskName non-const), so it stays.
// cppcheck-suppress constParameterPointer
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void) xTask;
    taskDISABLE_INTERRUPTS();
    Recovery::recordFault(Recovery::BootPhase::StackOverflowFault, pcTaskName);
    NVIC_SystemReset();
}

// Runs at the point of failure, in the context of whichever task asked for the memory
// -- so the scheduler is still running and the other tasks are still runnable.
// Signalling without stopping them would leave the higher-priority tasks transmitting
// telemetry from a firmware that is out of memory, which from the ground is
// indistinguishable from one that works. So interrupts go first.
//
// Before the scheduler starts there is no calling task to name, and pcTaskGetName(NULL)
// would not say so: once the first xTaskCreateStatic has run, pxCurrentTCB points at a
// created task even though none is running. The scheduler state is what tells the two
// apart, so an allocation from setup() is recorded as "setup".
extern "C" void vApplicationMallocFailedHook()
{
    taskDISABLE_INTERRUPTS();
    const char *taskName = (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        ? "setup"
        : pcTaskGetName(NULL);
    Recovery::recordFault(Recovery::BootPhase::MallocFailedFault, taskName);
    NVIC_SystemReset();
}
