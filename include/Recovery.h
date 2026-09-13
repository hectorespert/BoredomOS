#ifndef BOREDOMOS_RECOVERY_H
#define BOREDOMOS_RECOVERY_H

#include <stdint.h>

// Layout of R_SYSTEM->VBTBKR, the RA4M1's 512-byte battery-backed register file.
// Bytes [0..3] belong to the Arduino core's bootloader -- cores/arduino/boot.h's
// BOOT_DOUBLE_TAP_DATA, a 32-bit double-tap magic -- and are never read or written
// here. This firmware's own block starts at [4]. See design.md's layout table
// under openspec/changes/add-degraded-mode for the audit that established this.
//
// src/main.cpp writes every field here as the composition root for the boot
// decision; src/mavlink.cpp reads them to fill the heartbeat. Neither owns the
// definition, so it lives here, the same way include/Link.h and include/Data.h
// are shared without owning behaviour.
namespace Recovery {

// Marks [OFFSET_CONSECUTIVE_COUNT..OFFSET_DELIBERATE] as this firmware's own
// content rather than whatever a power event or an SEU left behind.
constexpr uint8_t VALIDITY_MAGIC = 0xB5;

enum class ResetReason : uint8_t {
    PowerOn = 0,
    LowVoltage = 1,
    Watchdog = 2,
    Software = 3,
    ExternalUnknown = 4,
    BackupStateInvalid = 5,
};

// Milestones of setup(), in the order they are reached, plus the two fault-hook
// markers. A reset whose next boot reads one of the first seven names the
// initialisation step that was in progress; StackOverflowFault and
// MallocFailedFault name a fault hook instead.
//
// LinkDone comes before ClockDone: both the heartbeat and the boot STATUSTEXT
// need LINK_SERIAL.begin() to have run, so main.cpp brings the link up first --
// see design.md's phase-marker decision (review.md finding 13). The numeric
// values below follow that order; do not reorder them to match an earlier
// draft without also fixing every setPhase() call site in main.cpp.
enum class BootPhase : uint8_t {
    Start = 0,
    LinkDone = 1,
    ClockDone = 2,
    CardDone = 3,
    QueuesDone = 4,
    TasksDone = 5,
    SchedulerStarted = 6,
    StackOverflowFault = 7,
    MallocFailedFault = 8,
};

// Whether the next boot's software reset was one this firmware chose to perform,
// as opposed to one that followed a fault. Consumed once by the boot that follows
// it, so that leaving the reduced configuration does not put the board straight
// back into it -- see design.md's deliberate-reset-carve-out decision.
enum class DeliberateReset : uint8_t {
    None = 0,
    Commanded = 1,
    Retry = 2,
};

// Byte offsets within VBTBKR. [0..3] is the bootloader's.
constexpr uint8_t OFFSET_VALIDITY_MAGIC = 4;
constexpr uint8_t OFFSET_CHECKSUM = 5;
constexpr uint8_t OFFSET_CONSECUTIVE_COUNT = 6;
constexpr uint8_t OFFSET_CUMULATIVE_COUNT = 7;
constexpr uint8_t OFFSET_CUMULATIVE_SNAPSHOT = 8;
constexpr uint8_t OFFSET_PHASE = 9;
constexpr uint8_t OFFSET_REASON = 10;
constexpr uint8_t OFFSET_DELIBERATE = 11;

constexpr uint8_t CONSECUTIVE_THRESHOLD = 3;
constexpr uint8_t CUMULATIVE_THRESHOLD = 10;

// True when the validity magic and the checksum over
// [OFFSET_CONSECUTIVE_COUNT..OFFSET_DELIBERATE] both match what is stored. False
// means the block is uninitialised (the first boot after flashing this firmware)
// or was corrupted in place (an SEU, or a brownout interrupting a write) and its
// content must not be trusted as counters.
bool isValid();

// Zeroes [OFFSET_CONSECUTIVE_COUNT..OFFSET_DELIBERATE] and writes a fresh magic
// and checksum over that all-zero content, without touching [0..3]. Called once,
// at the top of setup(), when isValid() is false -- see main.cpp. Also used by
// the ground-commanded return to normal (task 8.1) and by commissioning (9.8) to
// clear both counters and the snapshot together.
void reinitialise();

// Consecutive-unstable-boot and cumulative-reset counters. Both read as 0 when
// !isValid(). These are the true accumulated counts, NOT clamped to
// CONSECUTIVE_THRESHOLD / CUMULATIVE_THRESHOLD: the automatic retry's
// snapshot comparison (getCumulativeSnapshot() below) needs real growth beyond
// the threshold to tell "only the retry's own reset happened" from "a new fault
// happened while already saturated at the cap" -- clamping here would make the
// two indistinguishable once the raw count reaches the threshold. Clamp at the
// point of use instead: a threshold check is `count >= THRESHOLD`, which does
// not care how far past it the count is, and the heartbeat packs
// `min(count, THRESHOLD)` for display, in src/mavlink.cpp.
uint8_t getConsecutiveCount();
void setConsecutiveCount(uint8_t value);
uint8_t getCumulativeCount();
void setCumulativeCount(uint8_t value);

// The cumulative count recorded at the moment the reduced configuration was last
// entered. The automatic retry (src/mavlink.cpp's TaskHeartbeat) compares the
// *current* cumulative count against this snapshot rather than against
// CUMULATIVE_THRESHOLD, so the retry's own reset does not immediately re-select
// the reduced configuration -- see design.md finding 3.
uint8_t getCumulativeSnapshot();
void setCumulativeSnapshot(uint8_t value);

BootPhase getPhase();
void setPhase(BootPhase phase);

ResetReason getReason();
void setReason(ResetReason reason);

// Reads the deliberate-reset marker and clears it to None in the same call, so it
// is consumed exactly once by the boot that follows the reset it describes.
DeliberateReset consumeDeliberateReset();
void setDeliberateReset(DeliberateReset value);

} // namespace Recovery

#endif //BOREDOMOS_RECOVERY_H
