#include <Arduino.h>
#include <SD.h>
#include <Arduino_FreeRTOS.h>
#include <Priority.h>
#include <Link.h>
#include <MAVLink.h>
#include <Data.h>
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
bool systemTimeAvailable = false;
bool sdCardAvailable = false;
bool reducedConfiguration = false;

// The phase the *previous* boot reached before it reset -- captured before this
// boot overwrites Recovery's phase field with its own progress. This is what
// the heartbeat and the boot STATUSTEXT report; Recovery::getPhase() itself, by
// the time any task runs, reflects this boot's own milestones instead.
Recovery::BootPhase previousBootPhase = Recovery::BootPhase::Start;
Recovery::ResetReason previousResetReason = Recovery::ResetReason::PowerOn;

TaskHandle_t taskStatusHandler = NULL;

TaskHandle_t taskSerialWriteHandler = NULL;

TaskHandle_t taskSerialReadHandler = NULL;

TaskHandle_t taskHeartbeatHandler = NULL;

TaskHandle_t taskLoggerHandler = NULL;

TaskHandle_t taskSdWriteHandler = NULL;

TaskHandle_t taskMavlinkHandler = NULL;

QueueHandle_t serialReadQueue = NULL;

QueueHandle_t serialWriteQueue = NULL;

// Static storage for every task the scheduler will run. The word counts are the ones
// xTaskCreateStatic is given below, and together with the queue storage further down
// they are this firmware's RAM budget: the linker counts each array by name, so a
// build that does not fit fails here rather than on the board. Every array is declared
// unconditionally, whether or not the reduced configuration starts that task, so the
// RAM commitment does not change with the boot decision -- see design.md and task 5.3.
StackType_t serialReadStack[96];
StaticTask_t serialReadTcb;

StackType_t serialWriteStack[192];
StaticTask_t serialWriteTcb;

StackType_t heartbeatStack[128];
StaticTask_t heartbeatTcb;

StackType_t mavlinkStack[256];
StaticTask_t mavlinkTcb;

StackType_t loggerStack[96];
StaticTask_t loggerTcb;

StackType_t statusStack[128];
StaticTask_t statusTcb;

StackType_t sdWriteStack[256];
StaticTask_t sdWriteTcb;

// Queue structures and item storage. Each queue carries pointers, so the storage is
// depth x sizeof(pointer); what backs the items themselves is the FreeRTOS heap,
// sized in platformio.ini against the worst case computed in the change design.
StaticQueue_t sdWriteQueueBuffer;
uint8_t sdWriteQueueStorage[4 * sizeof(Data*)];

StaticQueue_t serialReadQueueBuffer;
uint8_t serialReadQueueStorage[8 * sizeof(mavlink_message_t*)];

StaticQueue_t serialWriteQueueBuffer;
uint8_t serialWriteQueueStorage[4 * sizeof(mavlink_message_t*)];

QueueHandle_t sdWriteQueue = NULL;

[[noreturn]] extern void TaskSerialWrite(void *pvParameters);

[[noreturn]] extern void TaskSerialRead(void *pvParameters);

[[noreturn]] extern void TaskHeartbeat(void *pvParameters);

[[noreturn]] extern void TaskLogger(void *pvParameters);

[[noreturn]] extern void TaskSdWrite(void *pvParameters);

[[noreturn]] extern void TaskSensors(void *pvParameters);

[[noreturn]] extern void TaskMavlinkBatteryStatus(void *pvParameters);

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

  // The console (Serial) is already open: the core's main() calls
  // Serial.begin(115200) before setup(). Opening it again here would
  // initialise the same port twice whenever LINK_SERIAL is overridden onto it.
  //
  // The link comes up before the clock or the card (review.md finding 13):
  // both the heartbeat and the boot STATUSTEXT need it, and a hang at an
  // earlier milestone would otherwise be a permanent, undiagnosable reset loop
  // on every boot, reduced included.
  LINK_SERIAL.begin(LINK_BAUD);
  Recovery::setPhase(Recovery::BootPhase::LinkDone);

  systemTimeAvailable = systemTime.begin();
  Recovery::setPhase(Recovery::BootPhase::ClockDone);

  sdCardAvailable = SD.begin(9);
  Recovery::setPhase(Recovery::BootPhase::CardDone);

  sdWriteQueue = xQueueCreateStatic(4, sizeof(Data*), sdWriteQueueStorage, &sdWriteQueueBuffer);
  configASSERT(sdWriteQueue != NULL);

  serialReadQueue = xQueueCreateStatic(8, sizeof(mavlink_message_t*), serialReadQueueStorage, &serialReadQueueBuffer);
  configASSERT(serialReadQueue != NULL);

  serialWriteQueue = xQueueCreateStatic(4, sizeof(mavlink_message_t*), serialWriteQueueStorage, &serialWriteQueueBuffer);
  configASSERT(serialWriteQueue != NULL);

  Recovery::setPhase(Recovery::BootPhase::QueuesDone);

  // With static storage these cannot fail for want of memory, so a NULL handle means
  // an argument is wrong -- a programming error, and worth trapping at boot.
  //
  // The link reader, the link writer, the heartbeat and the protocol handler start
  // in every configuration, reduced included -- see the "reachable and commandable"
  // requirement in specs/fault-recovery/spec.md. Housekeeping (the logger and the SD
  // writer) and battery telemetry do not start when reduced, and the logger and the
  // SD writer additionally do not start with no card, regardless of configuration.
  taskSerialReadHandler = xTaskCreateStatic(TaskSerialRead, "SerialRead", 96, NULL, PRIORITY_HIGHEST, serialReadStack, &serialReadTcb);
  configASSERT(taskSerialReadHandler != NULL);

  taskSerialWriteHandler = xTaskCreateStatic(TaskSerialWrite, "SerialWrite", 192, NULL, PRIORITY_HIGH, serialWriteStack, &serialWriteTcb);
  configASSERT(taskSerialWriteHandler != NULL);

  taskHeartbeatHandler = xTaskCreateStatic(TaskHeartbeat, "Heartbeat", 128, NULL, PRIORITY_HIGH, heartbeatStack, &heartbeatTcb);
  configASSERT(taskHeartbeatHandler != NULL);

  taskMavlinkHandler = xTaskCreateStatic(TaskMavlink, "Mavlink", 256, NULL, PRIORITY_LOW, mavlinkStack, &mavlinkTcb);
  configASSERT(taskMavlinkHandler != NULL);

  if (!reducedConfiguration) {
    taskStatusHandler = xTaskCreateStatic(TaskMavlinkBatteryStatus, "MavlinkBatteryStatus", 128, NULL, PRIORITY_HIGH, statusStack, &statusTcb);
    configASSERT(taskStatusHandler != NULL);

    if (sdCardAvailable) {
      taskLoggerHandler = xTaskCreateStatic(TaskLogger, "Logger", 96, NULL, PRIORITY_LOW, loggerStack, &loggerTcb);
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
