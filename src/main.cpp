#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <Link.h>
#include <LinkPort.h>
#include <MAVLink.h>
#include <LinkMsg.h>
#include <SdRecord.h>
#include <Battery.h>
#include <SystemTime.h>
#include <Recovery.h>
#include "r_wdt.h"

#ifndef WDT_TIMEOUT_MS
// Deliberately short for now -- task 4.6 raises this once TaskSdWrite's worst
// case (the ring rollover) has been measured on the board. Ceiling is 5.592 s;
// see design.md finding 5.
#define WDT_TIMEOUT_MS 1398
#endif

Battery battery = Battery();

SystemTime systemTime = SystemTime();

// Set once in setup() and read by src/mavlink.cpp for the heartbeat and the boot
// STATUSTEXT. Not task state: these describe what setup() found, once, at boot.
//
// There is no systemTimeAvailable here any more. It was written once and read
// nowhere, which is what an absent notion of clock provenance looked like; what
// setup() found about the clock now lives in SystemTime itself, as a source
// src/mavlink.cpp asks for, with four values instead of a boolean's two.
bool sdCardAvailable = false;
bool reducedConfiguration = false;

// The phase the *previous* boot reached before it reset -- captured before this
// boot overwrites Recovery's phase field with its own progress. This is what
// the heartbeat and the boot STATUSTEXT report; Recovery::getPhase() itself, by
// the time any task runs, reflects this boot's own milestones instead.
Recovery::BootPhase previousBootPhase = Recovery::BootPhase::Start;
Recovery::ResetReason previousResetReason = Recovery::ResetReason::PowerOn;

TaskHandle_t taskUartWriteHandler = NULL;

TaskHandle_t taskUartReadHandler = NULL;

TaskHandle_t taskUsbWriteHandler = NULL;

TaskHandle_t taskUsbReadHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

// One inbound queue shared by both readers, tagged with the port each item
// arrived on; one outbound queue per port, because the two writers must drain
// independently (design.md, Decision 1).
QueueHandle_t linkReadQueue = NULL;

QueueHandle_t uartWriteQueue = NULL;

QueueHandle_t usbWriteQueue = NULL;

// Static storage for every task the scheduler will run. The word counts are the ones
// xTaskCreateStatic is given below, and together with the queue storage further down
// they are this firmware's RAM budget: the linker counts each array by name, so a
// build that does not fit fails here rather than on the board. Every array is declared
// unconditionally, whether or not the reduced configuration starts that task, so the
// RAM commitment does not change with the boot decision -- see design.md and task 5.3.
// Measured at 30 of 96 words free (31%) after a 5-minute soak -- the thinnest
// of the link tasks, and below the 35-46 band recorded before this change,
// though that band was read from short runs rather than a soak. Left at 96:
// still inside the fleet's range (TaskSdWrite runs at 21%) and this change did
// not touch the reader's own locals.
StackType_t uartReadStack[96];
StaticTask_t uartReadTcb;

// 128, not the UART reader's 96, despite running the identical body: measured
// on the board at 27 of 96 words free against UartRead's 40, because
// _SerialUSB::available()/read() reach TinyUSB through deeper call frames than
// UART's do. 128 restores a margin comparable to the rest of the fleet (59 of
// 128, 46%). The parsed message is in the LinkPort descriptor rather than on
// either reader's stack, so neither carries a mavlink_message_t (design.md,
// Decision 7).
StackType_t usbReadStack[128];
StaticTask_t usbReadTcb;

// 384, not 192: queue-mavlink-messages-by-value added a local
// mavlink_message_t (291 B) that TaskLinkWrite fills via mavlinkPack()
// before packing to wire bytes, on top of the existing
// uint8_t buf[MAVLINK_MAX_PACKET_LEN] (280 B) and the received LinkMsg
// (64 B). 192 words (768 B) overflowed on the board within seconds of boot
// (the boot STATUSTEXT is the first message TaskLinkWrite ever packs) --
// confirmed by the slow 2s-on/2s-off LED pattern src/hooks.cpp's stack
// overflow hook produces. 384 measured at 145 of 384 free (38%) after a
// 5-minute soak (replace-console-cli-with-usb-mavlink-link).
StackType_t uartWriteStack[384];
StaticTask_t uartWriteTcb;

// Same body and the same locals as the UART writer -- a mavlink_message_t
// (291 B), a MAVLINK_MAX_PACKET_LEN buffer (280 B) and a LinkMsg (64 B), all
// still on the stack (design.md, Decision 7 deferred moving them). Measured at
// 146 of 384 free (38%), level with the UART writer as expected.
StackType_t usbWriteStack[384];
StaticTask_t usbWriteTcb;

// 384, not 256: queue-mavlink-messages-by-value added a local
// mavlink_message_t (291 B) to TaskMavlink's receive loop, alive for the
// whole loop body -- including every send*() call in the schedule pass below
// it, each of which has its own LinkMsg local on top. Measured on the board
// right after this change: 41 of 256 words free, down from the ~127
// add-mavlink-housekeeping-telemetry recorded -- thinner than acceptable
// margin, and before exercising the COMMAND_LONG branches at all. Grown to
// 384, and measured there at 155 free (40%) after a 5-minute soak carrying
// both ports' schedules (replace-console-cli-with-usb-mavlink-link).
StackType_t mavlinkStack[384];
StaticTask_t mavlinkTcb;

// 160, not 96. TaskLogger builds a Data on its stack, and
// replace-console-cli-with-usb-mavlink-link took sizeof(Data) from 36 B to 44 B
// by splitting the link tasks per port -- which showed up on the board as this
// task's high-water mark falling from 6 of 96 words free to 4, the two words
// the struct gained. 4 words is 16 bytes from a silent overflow, so the change
// that consumed them restores the margin rather than logging the regression and
// moving on: 160 measured at 68 free (42%) after a 5-minute soak.
//
// This does not close TODO.md's "TaskLogger's stack margin is critically tight",
// which asks why a 1 Hz sampling loop needs 92 words at all. It only undoes the
// damage this change did to it.
StackType_t loggerStack[160];
StaticTask_t loggerTcb;

StackType_t sdWriteStack[256];
StaticTask_t sdWriteTcb;

// Queue structures and item storage. EVERY queue now carries its items by value,
// so each one's storage is depth x sizeof(item) in .bss and none of them touches
// the FreeRTOS heap -- which, after sdWriteQueue stopped carrying a heap pointer,
// has no users left at all. There is no producer/consumer margin to add on top of
// the depth: with a by-value queue an item held before a send or after a receive
// is a local on that task's own stack, not a shared block.
StaticQueue_t sdWriteQueueBuffer;
uint8_t sdWriteQueueStorage[4 * sizeof(SdRecord)];

StaticQueue_t linkReadQueueBuffer;
uint8_t linkReadQueueStorage[8 * sizeof(InboundMsg)];

// Depth 7, re-derived for emit-sys-status rather than carried over. The six
// that came before (re-derived for improve-clock-synchronisation): heartbeat,
// SYSTEM_TIME, battery status and housekeeping -- four independently-clocked
// TaskMavlink schedule entries that nothing in the schedule stops from
// coinciding on one pass -- plus a TIMESYNC reply, which an inbound request
// can make due at any moment, plus the clock report: TaskMavlink posts TWO
// texts back to back at boot (the reset reason and the clock's origin) before
// its schedule has fired anything, and one more later whenever the origin
// changes.
//
// The seventh is SYS_STATUS: a fifth independently-clocked periodic entry
// (src/mavlink.cpp's schedule table) that the same "nothing stops it
// coinciding with the rest" reasoning applies to -- it is unconditional, like
// HEARTBEAT and SYSTEM_TIME, so it cannot be excluded from the worst case the
// way BATTERY_STATUS's reduced-configuration gating lets that one be. At
// depth 6 a pass where all five periodic entries are due, with a TIMESYNC
// request arriving, would make the seventh item a silent drop -- exactly the
// write-queue drop emit-sys-status's own `errors_count1` exists to count, so
// shipping it without this would let the counter observe drops it could have
// prevented. Found while writing this change's own tasks.md documentation
// step, not by a separate review pass.
//
// Costs 2 * sizeof(LinkMsg) = 128 B of .bss across the two ports, against the
// headroom scripts/ram_budget.py prints. Re-derived, not assumed, per CLAUDE.md's
// rule on changing a queue's backing. Each port gets its own queue at this depth.
// This depth and the housekeeping cycle length both follow the task count in this
// file -- a new task needs both re-checked.
StaticQueue_t uartWriteQueueBuffer;
uint8_t uartWriteQueueStorage[7 * sizeof(LinkMsg)];

StaticQueue_t usbWriteQueueBuffer;
uint8_t usbWriteQueueStorage[7 * sizeof(LinkMsg)];

QueueHandle_t sdWriteQueue = NULL;

// One body per direction, two instances of each: the LinkPort passed through
// pvParameters is what tells an instance which port it serves. This is the
// first thing in this firmware to use pvParameters at all -- see design.md,
// Decision 4, and the amendment it makes to ARCHITECTURE.md section 3.
[[noreturn]] extern void TaskLinkWrite(void *pvParameters);

[[noreturn]] extern void TaskLinkRead(void *pvParameters);

extern LinkPort linkPorts[2];
extern void linkPortsInit(QueueHandle_t uartWriteQueue, QueueHandle_t usbWriteQueue);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskMavlink(void *pvParameters);

namespace {

constexpr uint16_t kPrcrUnlockPrc1 = 0xA502;
constexpr uint16_t kPrcrLock = 0xA500;

// Reads RSTSR0/RSTSR1, decodes the reason, then clears every flag in both
// registers using the confirm-1-then-write-0 idiom the RA4M1's own register
// definition documents for all of them (including PORF -- review.md finding 3b,
// resolved from the RA4M1 CMSIS header, not assumed). RSTSR2.CWSF is set, not
// cleared, for the next boot: it is software-set rather than software-cleared.
// See design.md's reset-reason decision (review.md findings 3b, 3c, 14).
Recovery::ResetReason decodeAndClearResetReason()
{
    bool watchdog = R_SYSTEM->RSTSR1_b.IWDTRF || R_SYSTEM->RSTSR1_b.WDTRF;
    bool software = R_SYSTEM->RSTSR1_b.SWRF;
    bool lowVoltage = R_SYSTEM->RSTSR0_b.LVD0RF || R_SYSTEM->RSTSR0_b.LVD1RF || R_SYSTEM->RSTSR0_b.LVD2RF;
    bool powerOn = R_SYSTEM->RSTSR0_b.PORF;

    Recovery::ResetReason reason;
    if (watchdog) {
        reason = Recovery::ResetReason::Watchdog;
    } else if (software) {
        reason = Recovery::ResetReason::Software;
    } else if (lowVoltage) {
        reason = Recovery::ResetReason::LowVoltage;
    } else if (powerOn) {
        reason = Recovery::ResetReason::PowerOn;
    } else {
        reason = Recovery::ResetReason::ExternalUnknown;
    }

    R_SYSTEM->PRCR = kPrcrUnlockPrc1;
    if (R_SYSTEM->RSTSR0_b.PORF) R_SYSTEM->RSTSR0_b.PORF = 0;
    if (R_SYSTEM->RSTSR0_b.LVD0RF) R_SYSTEM->RSTSR0_b.LVD0RF = 0;
    if (R_SYSTEM->RSTSR0_b.LVD1RF) R_SYSTEM->RSTSR0_b.LVD1RF = 0;
    if (R_SYSTEM->RSTSR0_b.LVD2RF) R_SYSTEM->RSTSR0_b.LVD2RF = 0;
    if (R_SYSTEM->RSTSR0_b.DPSRSTF) R_SYSTEM->RSTSR0_b.DPSRSTF = 0;
    if (R_SYSTEM->RSTSR1_b.IWDTRF) R_SYSTEM->RSTSR1_b.IWDTRF = 0;
    if (R_SYSTEM->RSTSR1_b.WDTRF) R_SYSTEM->RSTSR1_b.WDTRF = 0;
    if (R_SYSTEM->RSTSR1_b.SWRF) R_SYSTEM->RSTSR1_b.SWRF = 0;
    R_SYSTEM->RSTSR2_b.CWSF = 1;
    R_SYSTEM->PRCR = kPrcrLock;

    return reason;
}

// Advances the counters for this boot and decides the task set, per design.md's
// deliberate-reset carve-out (review.md finding 4) and the cumulative-count-trap
// fix (review.md finding 3).
bool updateCountersAndDecideConfiguration(Recovery::ResetReason reason)
{
    Recovery::DeliberateReset deliberate = Recovery::consumeDeliberateReset();

    bool advancesConsecutive =
        (reason == Recovery::ResetReason::Watchdog || reason == Recovery::ResetReason::Software)
        && deliberate == Recovery::DeliberateReset::None;

    uint8_t consecutive = Recovery::getConsecutiveCount();
    uint8_t cumulative = Recovery::getCumulativeCount();

    if (advancesConsecutive) {
        consecutive++;
        Recovery::setConsecutiveCount(consecutive);
    }
    // Every reset that reaches this line -- power-on, low-voltage, external,
    // watchdog or software, deliberate or not -- is recorded in the cumulative
    // count; only the consecutive count distinguishes deliberate resets. See
    // design.md's counter-ownership decision.
    cumulative++;
    Recovery::setCumulativeCount(cumulative);

    bool overThreshold = consecutive >= Recovery::CONSECUTIVE_THRESHOLD
        || cumulative >= Recovery::CUMULATIVE_THRESHOLD;

    bool startReduced;
    if (!overThreshold) {
        startReduced = false;
    } else if (deliberate == Recovery::DeliberateReset::Retry
               && cumulative <= (uint16_t)Recovery::getCumulativeSnapshot() + 1) {
        // Only the retry's own reset (just counted above) grew the cumulative
        // count since it was last snapshotted: no further fault occurred, so
        // the retry succeeds -- review.md finding 3.
        startReduced = false;
    } else {
        startReduced = true;
    }

    if (startReduced) {
        Recovery::setCumulativeSnapshot(cumulative);
    }

    return startReduced;
}

// The RA4M1 WDT expresses 7 timeout counts x 10 clock divisions = 70 combinations
// (see design.md finding 5). PCLKB is 24 MHz (BSP_CFG_ICLK_DIV /1, BSP_CFG_PCLKB_DIV
// /2 of a 48 MHz ICLK), giving a ceiling of 16384 cycles / 8192 divisor = 5.592 s.
constexpr uint32_t kPclkbHz = 24000000;

struct WdtTimeoutOption { wdt_timeout_t timeout; uint32_t cycles; };
struct WdtDivisionOption { wdt_clock_division_t division; uint32_t divisor; };

constexpr WdtTimeoutOption kTimeoutOptions[] = {
    {WDT_TIMEOUT_128, 128}, {WDT_TIMEOUT_512, 512}, {WDT_TIMEOUT_1024, 1024},
    {WDT_TIMEOUT_2048, 2048}, {WDT_TIMEOUT_4096, 4096}, {WDT_TIMEOUT_8192, 8192},
    {WDT_TIMEOUT_16384, 16384},
};

constexpr WdtDivisionOption kDivisionOptions[] = {
    {WDT_CLOCK_DIVISION_1, 1}, {WDT_CLOCK_DIVISION_4, 4}, {WDT_CLOCK_DIVISION_16, 16},
    {WDT_CLOCK_DIVISION_32, 32}, {WDT_CLOCK_DIVISION_64, 64}, {WDT_CLOCK_DIVISION_128, 128},
    {WDT_CLOCK_DIVISION_256, 256}, {WDT_CLOCK_DIVISION_512, 512},
    {WDT_CLOCK_DIVISION_2048, 2048}, {WDT_CLOCK_DIVISION_8192, 8192},
};

// Picks the smallest of the 70 expressible (cycles, divisor) combinations whose
// period is at least requestedMs, so the watchdog never fires sooner than asked.
// Halts via configASSERT if requestedMs exceeds the 5.592 s ceiling -- that is a
// build-time mistake (an over-large WDT_TIMEOUT_MS), not a runtime fault.
void selectWdtTiming(uint32_t requestedMs, wdt_timeout_t &timeoutOut, wdt_clock_division_t &divisionOut)
{
    uint64_t bestPeriodUs = 0;
    bool found = false;
    for (const WdtTimeoutOption &t : kTimeoutOptions) {
        for (const WdtDivisionOption &d : kDivisionOptions) {
            uint64_t periodUs = (uint64_t)t.cycles * d.divisor * 1000000ULL / kPclkbHz;
            if (periodUs >= (uint64_t)requestedMs * 1000ULL && (!found || periodUs < bestPeriodUs)) {
                found = true;
                bestPeriodUs = periodUs;
                timeoutOut = t.timeout;
                divisionOut = d.division;
            }
        }
    }
    configASSERT(found);
}

} // namespace

// Shared with src/hooks.cpp's vApplicationIdleHook(), which refreshes it -- the
// control block has to outlive setup() and be the same instance every refresh.
wdt_instance_ctrl_t wdtCtrl;

void setup()
{
  // Captured before anything below overwrites it with this boot's own
  // progress -- see the previousBootPhase declaration above.
  previousBootPhase = Recovery::getPhase();

  // The backup-register block is self-healing on its very first successful
  // write (Recovery::writeByte always refreshes the magic and checksum), but
  // that write must land on known-zero content, not on whatever an
  // uninitialised block or a corrupted one holds -- so this has to run before
  // anything else touches it. See design.md finding 3d and include/Recovery.h.
  bool backupStateWasInvalid = !Recovery::isValid();
  if (backupStateWasInvalid) {
    Recovery::reinitialise();
  }

  Recovery::ResetReason reason = decodeAndClearResetReason();
  // The backup-state loss is the more important fact to report for this one
  // boot: it supersedes whatever RSTSR decoded, since it means the counters
  // this firmware is about to update cannot be trusted as history either way.
  if (backupStateWasInvalid) {
    reason = Recovery::ResetReason::BackupStateInvalid;
  }
  previousResetReason = reason;
  Recovery::setReason(reason);

  reducedConfiguration = updateCountersAndDecideConfiguration(reason);

  Recovery::setPhase(Recovery::BootPhase::Start);

  // Both links come up before the clock or the card (review.md finding 13):
  // the heartbeat and the boot STATUSTEXT need them, and a hang at an earlier
  // milestone would otherwise be a permanent, undiagnosable reset loop on
  // every boot, reduced included.
  //
  // LINK_USB is not begun here. It is Serial, which the core's own main()
  // already opened with Serial.begin(115200) before setup() ran; opening it a
  // second time would initialise the same port twice. LINK_BAUD applies to the
  // UART alone -- a CDC port has no line rate.
  LINK_UART.begin(LINK_BAUD);
  Recovery::setPhase(Recovery::BootPhase::LinkDone);

  // Return value intentionally unused: what was found is systemTime.source(),
  // which src/mavlink.cpp reports at boot. A missing clock degrades rather than
  // halting, per specs/fault-recovery/spec.md.
  (void)systemTime.begin();
  Recovery::setPhase(Recovery::BootPhase::ClockDone);

  sdCardAvailable = SD.begin(9);
  Recovery::setPhase(Recovery::BootPhase::CardDone);

  sdWriteQueue = xQueueCreateStatic(4, sizeof(SdRecord), sdWriteQueueStorage, &sdWriteQueueBuffer);
  configASSERT(sdWriteQueue != NULL);

  linkReadQueue = xQueueCreateStatic(8, sizeof(InboundMsg), linkReadQueueStorage, &linkReadQueueBuffer);
  configASSERT(linkReadQueue != NULL);

  uartWriteQueue = xQueueCreateStatic(7, sizeof(LinkMsg), uartWriteQueueStorage, &uartWriteQueueBuffer);
  configASSERT(uartWriteQueue != NULL);

  usbWriteQueue = xQueueCreateStatic(7, sizeof(LinkMsg), usbWriteQueueStorage, &usbWriteQueueBuffer);
  configASSERT(usbWriteQueue != NULL);

  // Binds each descriptor to its concrete port and its write queue. Must run
  // before any link task is created: a task body dereferences its LinkPort on
  // the first line.
  linkPortsInit(uartWriteQueue, usbWriteQueue);

  Recovery::setPhase(Recovery::BootPhase::QueuesDone);

  // With static storage these cannot fail for want of memory, so a NULL handle means
  // an argument is wrong -- a programming error, and worth trapping at boot.
  //
  // Every handle set here has a matching entry in src/mavlink.cpp's
  // housekeeping task table, published over MAVLink as a NAMED_VALUE_INT
  // round-robin. Adding a task here means adding it there too, and
  // re-checking each write queue's depth above -- both follow the task
  // count (see that table's comment).
  //
  // All four link tasks and Mavlink -- which carries the protocol handler, the
  // heartbeat and the system-time/battery telemetry schedule for both ports --
  // start in every configuration, reduced included, so the board stays
  // reachable and commandable on either port: see that requirement in
  // specs/fault-recovery/spec.md. Mavlink's own schedule withholds
  // BATTERY_STATUS when reduced (src/mavlink.cpp); it is not a decision made
  // here. Housekeeping (the logger and the SD writer) does not start when
  // reduced, and additionally does not start with no card, regardless of
  // configuration.
  //
  // HIGHEST for the UART reader and HIGH for the USB one, and the difference
  // is the hardware's rather than a preference: D0/D1 has no flow control
  // wired, so bytes not drained in time are lost outright, while USB CDC NAKs
  // when its buffer fills and can only be delayed (design.md, Decision 2).
  taskUartReadHandler = xTaskCreateStatic(TaskLinkRead, "UartRead", 96, &linkPorts[0], PRIORITY_HIGHEST, uartReadStack, &uartReadTcb);
  configASSERT(taskUartReadHandler != NULL);

  taskUsbReadHandler = xTaskCreateStatic(TaskLinkRead, "UsbRead", 128, &linkPorts[1], PRIORITY_HIGH, usbReadStack, &usbReadTcb);
  configASSERT(taskUsbReadHandler != NULL);

  taskUartWriteHandler = xTaskCreateStatic(TaskLinkWrite, "UartWrite", 384, &linkPorts[0], PRIORITY_HIGH, uartWriteStack, &uartWriteTcb);
  configASSERT(taskUartWriteHandler != NULL);

  // HIGH is safe here only because TaskLinkWrite checks availableForWrite()
  // before writing: _SerialUSB::write() spins without yielding when its buffer
  // is full, which above idle priority would stop the watchdog being refreshed
  // (design.md, Decision 6).
  taskUsbWriteHandler = xTaskCreateStatic(TaskLinkWrite, "UsbWrite", 384, &linkPorts[1], PRIORITY_HIGH, usbWriteStack, &usbWriteTcb);
  configASSERT(taskUsbWriteHandler != NULL);

  taskMavlinkHandler = xTaskCreateStatic(TaskMavlink, "Mavlink", 384, NULL, PRIORITY_HIGH, mavlinkStack, &mavlinkTcb);
  configASSERT(taskMavlinkHandler != NULL);

  if (!reducedConfiguration) {
    if (sdCardAvailable) {
      taskLoggerHandler = xTaskCreateStatic(TaskLogger, "Logger", 160, NULL, PRIORITY_LOW, loggerStack, &loggerTcb);
      configASSERT(taskLoggerHandler != NULL);

      taskSdWriteHandler = xTaskCreateStatic(TaskSdWrite, "SdWrite", 256, NULL, PRIORITY_LOWEST, sdWriteStack, &sdWriteTcb);
      configASSERT(taskSdWriteHandler != NULL);
    }
  }

  Recovery::setPhase(Recovery::BootPhase::TasksDone);

  wdt_timeout_t wdtTimeout;
  wdt_clock_division_t wdtDivision;
  selectWdtTiming(WDT_TIMEOUT_MS, wdtTimeout, wdtDivision);

  wdt_cfg_t wdtCfg;
  wdtCfg.timeout = wdtTimeout;
  wdtCfg.clock_division = wdtDivision;
  wdtCfg.window_start = WDT_WINDOW_START_100;
  wdtCfg.window_end = WDT_WINDOW_END_0;
  wdtCfg.reset_control = WDT_RESET_CONTROL_RESET;
  // Sleep stops the count (WDT_STOP_CONTROL_ENABLE) unless told otherwise; this
  // firmware's idle hook sleeps whenever nothing is due, so the count must keep
  // running through that sleep for the timeout to mean wall time rather than
  // CPU-awake time -- review.md finding 17.
  wdtCfg.stop_control = WDT_STOP_CONTROL_DISABLE;
  configASSERT(R_WDT_Open(&wdtCtrl, &wdtCfg) == FSP_SUCCESS);

  Recovery::setPhase(Recovery::BootPhase::SchedulerStarted);

  vTaskStartScheduler();
}

void loop() {}
