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
  // CONCURRENCY: the clock write and the epoch update in setUnixTime() are two
  // stores and cannot be made one, so a reader landing between them would get a
  // wrong answer. That is safe only while the single reader of these two
  // accessors is also their single writer -- TaskMavlink. Before a SECOND task
  // reads them (the planned DataFlash log is exactly that), the pair update
  // must be made indivisible by suspending the scheduler across it, not by
  // adding a mutex.
  uint64_t sinceBootUsec();
  int64_t sinceBootNsec();

  // Applies a time if it is plausible and does not come from a worse source
  // than the one currently held, and reports whether it did. Writes the DS1307
  // too when it is present, so the clock that seeds the next boot is corrected
  // as well -- which is what makes Ds1307 able to outrank Survived. The return
  // value is what a caller needs to know a clock set actually happened; the
  // planned DataFlash log emits its TIME record on it.
  bool setUnixTime(time_t unix_time, Source from);

  // Re-reads the DS1307 and brings the internal RTC back to it. Does nothing
  // once a time from the ground has been accepted, since Ground outranks
  // Ds1307 and setUnixTime() refuses the demotion.
  bool reseedFromDs1307();

  // A time at or after 2022-01-01T00:00:00Z. Serves twice: it validates what
  // arrives from the ground, and it is how begin() decides whether a
  // still-running internal RTC survived a reset holding something worth
  // keeping. An internal RTC that was never set reads as 1970 and fails it.
  static bool isPlausible(time_t unix_time);

private:
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
