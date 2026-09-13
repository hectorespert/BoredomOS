#include <Recovery.h>
#include <Arduino.h>

// PRCR-unlocked access to R_SYSTEM->VBTBKR[4..11]. This is the only file that
// writes those bytes; cores/arduino/boot.cpp's own PRCR unlock for VBTBKR[0..3]
// is a separate, unrelated critical section over the same register -- see
// design.md finding 7 for why every write here has to be one.
namespace {

constexpr uint16_t kPrcrUnlockPrc1 = 0xA502;
constexpr uint16_t kPrcrLock = 0xA500;

inline volatile uint8_t &vbtbkr(uint8_t offset)
{
    return R_SYSTEM->VBTBKR[offset];
}

// Sum over this firmware's counters and markers, excluding the magic and the
// checksum byte itself.
uint8_t computeChecksum()
{
    uint8_t sum = 0;
    for (uint8_t offset = Recovery::OFFSET_CONSECUTIVE_COUNT; offset <= Recovery::OFFSET_DELIBERATE; offset++) {
        sum += vbtbkr(offset);
    }
    return sum;
}

} // namespace

bool Recovery::isValid()
{
    return vbtbkr(OFFSET_VALIDITY_MAGIC) == VALIDITY_MAGIC
        && vbtbkr(OFFSET_CHECKSUM) == computeChecksum();
}

void Recovery::reinitialise()
{
    R_SYSTEM->PRCR = kPrcrUnlockPrc1;
    for (uint8_t offset = OFFSET_CONSECUTIVE_COUNT; offset <= OFFSET_DELIBERATE; offset++) {
        vbtbkr(offset) = 0;
    }
    vbtbkr(OFFSET_CHECKSUM) = computeChecksum();
    vbtbkr(OFFSET_VALIDITY_MAGIC) = VALIDITY_MAGIC;
    R_SYSTEM->PRCR = kPrcrLock;
}

namespace {

// Every external write goes through here: it stores the byte, then refreshes the
// magic and checksum so the block reads as valid immediately afterward. This is
// deliberate -- see the sequencing note in main.cpp's setup(): a caller that
// finds !isValid() must call Recovery::reinitialise() first, so the counters this
// recomputes the checksum over are known-zero rather than whatever was there
// before this firmware ever ran.
void writeByte(uint8_t offset, uint8_t value)
{
    R_SYSTEM->PRCR = kPrcrUnlockPrc1;
    vbtbkr(offset) = value;
    vbtbkr(Recovery::OFFSET_CHECKSUM) = computeChecksum();
    vbtbkr(Recovery::OFFSET_VALIDITY_MAGIC) = Recovery::VALIDITY_MAGIC;
    R_SYSTEM->PRCR = kPrcrLock;
}

} // namespace

uint8_t Recovery::getConsecutiveCount()
{
    return isValid() ? vbtbkr(OFFSET_CONSECUTIVE_COUNT) : 0;
}

void Recovery::setConsecutiveCount(uint8_t value)
{
    writeByte(OFFSET_CONSECUTIVE_COUNT, value);
}

uint8_t Recovery::getCumulativeCount()
{
    return isValid() ? vbtbkr(OFFSET_CUMULATIVE_COUNT) : 0;
}

void Recovery::setCumulativeCount(uint8_t value)
{
    writeByte(OFFSET_CUMULATIVE_COUNT, value);
}

uint8_t Recovery::getCumulativeSnapshot()
{
    return isValid() ? vbtbkr(OFFSET_CUMULATIVE_SNAPSHOT) : 0;
}

void Recovery::setCumulativeSnapshot(uint8_t value)
{
    writeByte(OFFSET_CUMULATIVE_SNAPSHOT, value);
}

Recovery::BootPhase Recovery::getPhase()
{
    return isValid() ? static_cast<BootPhase>(vbtbkr(OFFSET_PHASE)) : BootPhase::Start;
}

void Recovery::setPhase(BootPhase phase)
{
    writeByte(OFFSET_PHASE, static_cast<uint8_t>(phase));
}

Recovery::ResetReason Recovery::getReason()
{
    return isValid() ? static_cast<ResetReason>(vbtbkr(OFFSET_REASON)) : ResetReason::BackupStateInvalid;
}

void Recovery::setReason(ResetReason reason)
{
    writeByte(OFFSET_REASON, static_cast<uint8_t>(reason));
}

Recovery::DeliberateReset Recovery::consumeDeliberateReset()
{
    DeliberateReset value = isValid() ? static_cast<DeliberateReset>(vbtbkr(OFFSET_DELIBERATE)) : DeliberateReset::None;
    if (value != DeliberateReset::None) {
        writeByte(OFFSET_DELIBERATE, static_cast<uint8_t>(DeliberateReset::None));
    }
    return value;
}

void Recovery::setDeliberateReset(DeliberateReset value)
{
    writeByte(OFFSET_DELIBERATE, static_cast<uint8_t>(value));
}
