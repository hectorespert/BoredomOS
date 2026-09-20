#include <SystemTime.h>
#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <RTC.h>

// 2022-01-01T00:00:00Z, after ArduPilot's AP_RTC oldest_acceptable_date_us. A
// fixed constant rather than something derived from the build date, so the
// accepted range does not depend on when the firmware happened to be compiled.
static constexpr time_t kOldestAcceptable = 1640995200;

// R64CNT is driven by the same divider chain that carries into the seconds
// register, so a fraction read from it is phase-locked to the second it
// accompanies -- nothing has to be kept in step.
//
// SEVEN bits, not six, and 1/128 s per count -- which the register's name
// ("64-Hz Counter") actively misleads about. Its bits are named for the
// frequency at which each one TOGGLES: bit 0 is F64HZ, bit 5 is F2HZ and bit 6
// is F1HZ. A counter whose least significant bit toggles at 64 Hz increments at
// 128 Hz, and F1HZ toggling at 1 Hz means one whole second spans bits 0..6, so
// the sub-second value runs 0..127. Masking six bits makes it wrap twice a
// second, which sends the reported time back by almost a full second halfway
// through every second -- caught by the Unity case that hammers the accessor,
// and invisible to anything sampling at 1 Hz.
static constexpr uint8_t kR64CntMask = 0x7F;
static constexpr uint32_t kR64TicksPerSec = 128;
// 1e6 / 128 is 7812.5, so the halves are kept by scaling before dividing rather
// than losing 0.5 us per count (64 us per second at the top of the range).
static constexpr uint32_t kUsecPerR64TickNum = 15625;
static constexpr uint32_t kUsecPerR64TickDen = 2;

// The epoch is stored in a single 32-bit word so that its store cannot be read
// torn. Both halves of that claim are asserted rather than assumed.
static_assert(sizeof(uint32_t) == 4, "the boot epoch must be a single 32-bit word");
static_assert(kOldestAcceptable > 0 && (uint64_t)kOldestAcceptable < 0xFFFFFFFFull,
              "the plausibility floor must fit the 32-bit boot epoch");

SystemTime::SystemTime()
    : _ds1307(), _bootEpoch(0), _source(Source::None), _ds1307Present(false),
      _foundRunning(false)
{
}

bool SystemTime::isPlausible(time_t unix_time)
{
    // Upper bound as well as the floor: time_t is 8 bytes here and the boot epoch
    // is stored in 32 bits, so a value past 2106 would be accepted and then
    // silently truncated on the way in. Copilot's review of this change caught
    // that the range was open at the top.
    return unix_time >= kOldestAcceptable && (uint64_t)unix_time <= 0xFFFFFFFFull;
}

bool SystemTime::begin()
{
    _ds1307Present = _ds1307.begin();

    // Unconditional, where this used to be `_ds1307.begin() && RTC.begin()`: the
    // short-circuit meant a board with no DS1307 never started its internal RTC
    // at all, so there was no clock to read, no 64 Hz counter to sample, and the
    // degraded behaviour specs/fault-recovery/spec.md requires did not exist.
    // Its return value is not consulted -- the core's openRtc() returns true
    // from its failure branch as well, so it would say nothing.
    RTC.begin();

    // Taken before anything is written, or it would describe this boot's own
    // seeding rather than what the previous boot left behind.
    _foundRunning = RTC.isRunning() && isPlausible(getUnixTime());

    bool seededFromDs1307 = false;
    if (_ds1307Present) {
        time_t fromDs1307 = (time_t)_ds1307.now().unixtime();
        if (isPlausible(fromDs1307)) {
            RTCTime seed(fromDs1307);
            seededFromDs1307 = RTC.setTime(seed);
        }
    }

    if (seededFromDs1307) {
        _source = Source::Ds1307;
    } else if (_foundRunning) {
        _source = Source::Survived;
    } else {
        // Nothing seeded it, so start it counting from the epoch and say so.
        // getUnixTime() then returns time since boot, which is what
        // specs/fault-recovery/spec.md means by operating on time measured from
        // boot; what goes on the wire in this state is a zero, handled by the
        // caller rather than by lying about the reading here.
        RTCTime zero((time_t)0);
        RTC.setTimeIfNotRunning(zero);
        _source = Source::None;
    }

    _bootEpoch = (uint32_t)getUnixTime();
    return _source != Source::None;
}

bool SystemTime::readSeconds(time_t &out)
{
    RTCTime currentTime;
    if (!RTC.getTime(currentTime)) {
        return false;
    }
    out = currentTime.getUnixTime();
    return true;
}

time_t SystemTime::getUnixTime()
{
    time_t seconds = 0;
    return readSeconds(seconds) ? seconds : 0;
}

uint64_t SystemTime::getUnixTimeUsec()
{
    // A read can land across the seconds carry and pair a stale second with a
    // fresh fraction, which would report a time a whole second in the past --
    // the exact fault sub-second resolution exists to remove. Reading the
    // second either side of the fraction and retrying while it moved closes
    // that window. Bounded, not a spin: the second moves once per 128 counts,
    // so a second attempt already succeeds in practice and the cap only guards
    // against a clock that is not advancing sanely.
    // A failed read is reported as 0, but a SUCCESSFUL read of 0 is a real
    // reading: with no origin the internal RTC starts at the epoch and counts
    // from there, so it genuinely reads 0 for the first second of that
    // configuration. Keying off the value would make elapsed time stall for that
    // second, so the two are told apart by readSeconds()'s result instead.
    for (uint8_t attempt = 0; attempt < 4; ++attempt) {
        time_t before = 0;
        if (!readSeconds(before)) {
            return 0;
        }
        uint32_t fraction = (uint32_t)(R_RTC->R64CNT & kR64CntMask);
        time_t after = 0;
        if (!readSeconds(after)) {
            return 0;
        }
        if (before == after) {
            return (uint64_t)before * USEC_PER_SEC
                   + (fraction * kUsecPerR64TickNum) / kUsecPerR64TickDen;
        }
    }
    // Whole seconds are wrong by less than a second; a torn pair can be wrong by
    // a second in the wrong direction. Degrade to the former.
    return (uint64_t)getUnixTime() * USEC_PER_SEC;
}

int64_t SystemTime::getUnixTimeNsec()
{
    return (int64_t)(getUnixTimeUsec() * 1000ULL);
}

uint64_t SystemTime::sinceBootUsec()
{
    uint64_t now = getUnixTimeUsec();
    uint64_t base = (uint64_t)_bootEpoch * USEC_PER_SEC;
    return now > base ? now - base : 0;
}

int64_t SystemTime::sinceBootNsec()
{
    return (int64_t)(sinceBootUsec() * 1000ULL);
}

bool SystemTime::setUnixTime(time_t unix_time, Source from, bool *clockMoved)
{
    // Default to "the clock did not move" and set it true only on the one path that
    // writes the clock. Every early return below is a path where it did not.
    if (clockMoved != nullptr) {
        *clockMoved = false;
    }

    if (!isPlausible(unix_time)) {
        return false;
    }

    // Lower is better, so a numerically greater Source is a demotion. This is
    // what stops the periodic DS1307 re-seed from undoing a time the ground set,
    // and it is the whole content of the ladder at run time.
    if ((uint8_t)from > (uint8_t)_source) {
        return false;
    }

    time_t previous = getUnixTime();

    // A ground station repeating the second the clock already holds costs nothing.
    // This matters here specifically: MAVProxy's system_time module -- the
    // reference GCS's way of setting the clock -- sends SYSTEM_TIME once a second,
    // so without this the high-priority TaskMavlink would do a clock write and an
    // I2C read every second forever. The source is still promoted, because a time
    // from the ground is a time from the ground whether or not it moved anything.
    //
    // Correcting a DS1307 that has drifted under a ground-set clock is NOT lost by
    // returning early: that is what TaskMavlink's periodic reconciliation does, and
    // it is where a job measured in hours belongs. It was folding it into this path
    // that made every inbound message pay for it. Found by Copilot's review.
    if (previous == unix_time) {
        // A PROMOTION still reaches the DS1307 even on an equal second. Otherwise
        // an internal clock already on the right second, with a drifted DS1307
        // behind it, would leave the next boot seeding from the stale one -- and
        // the promotion is the message that carries new information, so it is the
        // one worth paying an I2C round trip for. A repeat from a source that
        // already holds the clock is the free case, and it is the one a 1 Hz
        // ground station generates. Copilot's third pass caught that collapsing
        // both into one early return contradicted this capability's own
        // requirement that an accepted time correct both clocks.
        bool promoting = (from != _source);
        _source = from;
        if (promoting && _ds1307Present && from != Source::Ds1307
            && (time_t)_ds1307.now().unixtime() != unix_time) {
            _ds1307.adjust(DateTime((uint32_t)unix_time));
        }
        return true;
    }

    RTCTime updated(unix_time);

    // The clock write and the epoch correction are ONE update as far as any reader
    // of sinceBootUsec() is concerned, and they are performed with the scheduler
    // suspended so that no reader can land between them. A reader that did would
    // compute the new wall clock against the old epoch and get an elapsed time
    // wrong by the size of the correction -- usually decades. The scheduler is
    // suspended rather than a mutex taken because this firmware's freedom from
    // mutexes rests on single ownership (ARCHITECTURE.md section 6) and the pair is
    // two stores; the header's note on this required it before a second task read
    // these accessors, and the DataFlash log made TaskLogger and TaskSdWrite exactly
    // that.
    //
    // Nothing that can block is inside the suspended region. RTC.setTime() reaches
    // R_RTC_CalendarTimeSet, which busy-waits on registers rather than yielding, and
    // the DS1307's I2C write is deliberately left outside it below -- I2C is the one
    // call here that could block, and it does not participate in the pair.
    vTaskSuspendAll();
    bool clockWritten = RTC.setTime(updated);
    if (clockWritten) {
        // The one path where the wall clock actually changed.
        if (clockMoved != nullptr) {
            *clockMoved = true;
        }

        // The correction applies to the epoch as well, so that time since boot is
        // continuous across it. Without this a clock set makes the elapsed measure
        // jump by the size of the correction. It is also what lets this firmware
        // accept a BACKWARDS correction at all, which ArduPilot refuses outright
        // because its wall clock and its monotonic timestamps are coupled and ours
        // are not.
        _bootEpoch = (uint32_t)((int64_t)_bootEpoch + ((int64_t)unix_time - (int64_t)previous));
    }
    xTaskResumeAll();

    if (!clockWritten) {
        return false;
    }

    // Skipped when the value came from the DS1307 -- writing it straight back
    // would be an I2C round trip to store what is already there -- and skipped
    // when the DS1307 already holds this second. Removing the old early return
    // was necessary (it was what stopped a drifted DS1307 from ever being
    // corrected), but removing it wholesale would let a peer sending plausible
    // times faster than 1 Hz make TaskMavlink block on an I2C WRITE each time.
    // A read to compare is the cheaper half of that round trip and keeps the
    // correction: an unchanged clock costs a read, a drifted one still gets
    // written. Found by Copilot's review of this change.
    if (_ds1307Present && from != Source::Ds1307
        && (time_t)_ds1307.now().unixtime() != unix_time) {
        _ds1307.adjust(DateTime((uint32_t)unix_time));
    }

    _source = from;
    return true;
}

bool SystemTime::reseedFromDs1307(bool *clockMoved)
{
    if (clockMoved != nullptr) {
        *clockMoved = false;
    }
    if (!_ds1307Present) {
        return false;
    }
    return setUnixTime((time_t)_ds1307.now().unixtime(), Source::Ds1307, clockMoved);
}

bool SystemTime::pushToDs1307()
{
    if (!_ds1307Present) {
        return false;
    }

    time_t internal = getUnixTime();
    if (!isPlausible(internal)) {
        return false;
    }
    if ((time_t)_ds1307.now().unixtime() == internal) {
        return false;
    }

    _ds1307.adjust(DateTime((uint32_t)internal));
    return true;
}
