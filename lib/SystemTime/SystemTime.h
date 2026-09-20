#ifndef SYSTEM_TIME_H
#define SYSTEM_TIME_H
#define USEC_PER_SEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
#include <RTClib.h>

class SystemTime
{
public:
  // Where the time this object holds came from. A LOWER value is a better
  // source, which is what setUnixTime() enforces -- modelled on ArduPilot's
  // AP_RTC::source_type, including its ordering: a time from the ground
  // outranks the battery-backed clock.
  //
  // The RA4M1's internal RTC is deliberately absent from this list. It is not a
  // source, it is the register that holds and advances the time -- the
  // counterpart of ArduPilot's rtc_shift, which likewise has no entry there.
  // What is ranked here is whatever last wrote into it. Survived is the one
  // case where the internal RTC does act as a source: it was already running,
  // with a plausible time, when this boot began.
  enum class Source : uint8_t {
    Ground = 0,
    Ds1307 = 1,
    Survived = 2,
    None = 3,
  };

  SystemTime();

  // Starts the internal RTC unconditionally -- not only when the DS1307
  // answers -- and selects the initial Source. Returns whether a usable wall
  // clock was found, i.e. source() != Source::None. That return value is NOT
  // evidence the internal RTC opened: the core's openRtc() returns true from
  // its failure branch too, which is why the Source is derived from
  // RTC.isRunning() plus a plausible reading instead.
  bool begin();

  Source source() const { return _source; }

  // Whether the internal RTC was ALREADY running with a plausible time when
  // begin() ran, independent of which Source won. Reported separately from
  // source() on purpose: Ds1307 outranks Survived, so on a board whose DS1307
  // cannot be disconnected the ladder would otherwise hide the fact that the
  // clock kept running across a reset. This is the only way that behaviour is
  // observable here -- see design.md.
  bool foundClockRunning() const { return _foundRunning; }

  // Whole seconds from the internal RTC. 0 when it cannot be read.
  time_t getUnixTime();

  // Wall clock with the sub-second part taken from the RTC's own R64CNT counter
  // (1/128 s, see the .cpp -- the register's name says 64 Hz and means
  // something else), so the fraction is phase-locked to the second it
  // accompanies. Stateless: safe to call from any task. Returns 0 when the clock
  // cannot be read.
  uint64_t getUnixTimeUsec();
  int64_t getUnixTimeNsec();

  // Time since boot, as wall clock minus the epoch latched by begin(). Carries
  // no accumulator and cannot wrap. Monotonic across a clock set, because
  // setUnixTime() shifts the epoch by the same delta it applies to the clock.
  //
  // CONCURRENCY: safe to call from any task. The clock write and the epoch update
  // in setUnixTime() are two stores and cannot be made one, so a reader landing
  // between them would get an elapsed time wrong by the size of the correction.
  // They are therefore performed with the scheduler suspended, which makes the
  // pair indivisible without a mutex -- see setUnixTime() in the .cpp for why the
  // DS1307's I2C write stays outside that region. This used to hold only while the
  // single reader was also the single writer (TaskMavlink); the DataFlash log added
  // TaskLogger and TaskSdWrite as readers, which is what required closing it.
  uint64_t sinceBootUsec();
  int64_t sinceBootNsec();

  // Applies a time if it is plausible and does not come from a worse source
  // than the one currently held, and reports whether it did. Writes the DS1307
  // too when it is present, so the clock that seeds the next boot is corrected
  // as well -- which is what makes Ds1307 able to outrank Survived. The return
  // value says the time was ACCEPTED. It does not say the clock moved, and reading
  // it as though it did is a mistake this interface has already caused once.
  //
  // The optional clockMoved out-parameter reports whether the WALL CLOCK actually
  // changed, which the return value does not: this returns true for an accepted time
  // that equals the second already held, and the reference GCS offers a time every
  // second, so "accepted" and "changed" differ on almost every call. A caller that
  // needs the distinction -- the flight log emits its TIME record on a change, not on
  // an acceptance -- must use this rather than infer it from the bool. Set to false on
  // every path that does not write the clock, including both refusals.
  //
  // Deducing it by comparing getUnixTime() before and after does NOT work: a second
  // boundary crossed between the caller's read and this function's own makes an
  // unchanged clock look changed. Only this function knows which path it took.
  bool setUnixTime(time_t unix_time, Source from, bool *clockMoved = nullptr);

  // The two halves of the periodic reconciliation, which the caller picks between
  // by looking at source(). Neither belongs on the inbound-message path: keeping
  // the clocks in agreement is a job measured in hours.
  //
  // Re-reads the DS1307 and brings the internal RTC back to it. Does nothing once
  // a time from the ground has been accepted, since Ground outranks Ds1307 and
  // setUnixTime() refuses the demotion.
  // clockMoved, as on setUnixTime(): whether the wall clock actually changed, which
  // is what a drift correction does without changing the origin. The return value only
  // says the re-seed was accepted, and a DS1307 already on the held second accepts
  // without moving anything.
  bool reseedFromDs1307(bool *clockMoved = nullptr);

  // The other direction, for when the ground is the authority: writes the internal
  // clock out to the DS1307 so the next boot seeds from something current. This is
  // what stops a DS1307 drifting unnoticed under a ground-set clock, which used to
  // be handled -- expensively -- on every inbound SYSTEM_TIME.
  bool pushToDs1307();

  // A time at or after 2022-01-01T00:00:00Z, and within what the 32-bit boot
  // epoch can hold. Serves twice: it validates what
  // arrives from the ground, and it is how begin() decides whether a
  // still-running internal RTC survived a reset holding something worth
  // keeping. An internal RTC that was never set reads as 1970 and fails it.
  static bool isPlausible(time_t unix_time);

private:
  // Reads whole seconds and reports whether the read worked, which getUnixTime()
  // cannot: it returns 0 for a failure, and 0 is also a valid reading on a board
  // whose clock started at the epoch. The fraction path needs to tell those apart.
  bool readSeconds(time_t &out);

  RTC_DS1307 _ds1307;

  // Wall-clock seconds at boot. uint32_t, NOT time_t: time_t is 8 bytes on this
  // toolchain, so it would take two stores and could be read torn. UNIX seconds
  // fit unsigned 32-bit until 2106 and isPlausible() keeps the value far above
  // zero, so the narrowing is safe by construction.
  uint32_t _bootEpoch;

  Source _source;
  bool _ds1307Present;
  bool _foundRunning;
};

#endif
