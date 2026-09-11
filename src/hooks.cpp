#include <Arduino.h>
#include <Arduino_FreeRTOS.h>

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

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
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
