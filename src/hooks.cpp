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

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    // First statement, ahead of even taskDISABLE_INTERRUPTS() -- review.md
    // finding 9's ordering concern is noted for a future pass (this hook still
    // relies on the watchdog underflowing to reset the board, not on an
    // explicit NVIC_SystemReset() here), but writing the marker before
    // anything else at least makes it survive whichever comes first.
    Recovery::setPhase(Recovery::BootPhase::StackOverflowFault);
    taskDISABLE_INTERRUPTS();
    while (!Serial) {}
    Serial.println("Overflow on" + String(pcTaskName) + " task!");
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(2000);
        digitalWrite(LED_BUILTIN, LOW);
        delay(2000);
    }
}

// Spins for roughly the requested number of milliseconds without the scheduler tick.
// delay() cannot be used from a fault hook: it waits on a tick that has stopped along
// with the interrupts. The constant is approximate -- 48 MHz, and the loop is a few
// cycles per iteration -- because the blink only has to be legible, not accurate.
static void spinMilliseconds(uint32_t ms)
{
    for (uint32_t outer = 0; outer < ms; outer++) {
        for (volatile uint32_t inner = 0; inner < 6000; inner++) {
        }
    }
}

// heap_4 calls this at the point of failure, after xTaskResumeAll() and in the context
// of whichever task asked for the memory -- so the scheduler is still running and the
// other tasks are still runnable. Signalling without stopping them would leave the
// higher-priority tasks transmitting telemetry from a firmware that is out of memory,
// which from the ground is indistinguishable from one that works. So interrupts go
// first, and this never returns.
//
// Two rapid blinks and a long pause, to be told apart from the stack-overflow hook's
// slow symmetric 0.25 Hz. Both patterns are recorded in ARCHITECTURE.md section 6.
extern "C" void vApplicationMallocFailedHook()
{
    Recovery::setPhase(Recovery::BootPhase::MallocFailedFault);
    taskDISABLE_INTERRUPTS();
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
        for (uint8_t blink = 0; blink < 2; blink++) {
            digitalWrite(LED_BUILTIN, HIGH);
            spinMilliseconds(120);
            digitalWrite(LED_BUILTIN, LOW);
            spinMilliseconds(120);
        }
        spinMilliseconds(1000);
    }
}
