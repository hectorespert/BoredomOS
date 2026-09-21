#include <unity.h>
#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Battery.h>
#include <SystemTime.h>
#include <SD.h>
#include <SdData.h>

#define TEST_FILE_COUNT 4

// Bytes, and now named so. It was TEST_FILE_SIZE_MB with the same 1024 value, which is
// listed in TODO.md's "Minor leftovers cleanup" -- renamed here rather than there because
// this change had to touch the value anyway.
//
// 8 KiB, not 1 KiB, and the reason is coverage rather than taste: SdData syncs every
// FLUSH_INTERVAL_BYTES (4 KiB), so at a 1 KiB file size a rotation always arrived first
// and close() synced, and the batching path could not be exercised by this suite at all.
// A file twice the interval lets both be seen.
#define TEST_FILE_SIZE_BYTES 8192UL

Battery battery;
SystemTime systemTime;
SdData sdData(TEST_FILE_COUNT, TEST_FILE_SIZE_BYTES);

void cleanSdFiles() {
    for (int i = 0; i < TEST_FILE_COUNT; ++i) {
        String fname = "data" + String(i) + ".BIN";
        if (SD.exists(fname.c_str())) {
            SD.remove(fname.c_str());
        }
    }
    if (SD.exists("index.bin")) {
        SD.remove("index.bin");
    }
}

void setUp(void) {
    battery = Battery();
    systemTime = SystemTime();
    cleanSdFiles();
    sdData.begin();
}

void tearDown(void) {
  // end() BEFORE cleanSdFiles(), and the order is the whole protection: without it
  // cleanSdFiles() SD.remove()s a file sdData still holds open, and the close that
  // eventually follows syncs a directory entry that has already been freed. There is
  // deliberately no guard inside cleanSdFiles() -- that would need SdData to expose
  // which file it holds, a public accessor describing internal state with no other
  // caller. If these two lines are ever reordered, nothing will complain.
  sdData.end();
  cleanSdFiles();
}

void test_voltaje_should_return_battery_voltage(void) {
    float voltage = battery.voltage();
    TEST_ASSERT_TRUE(voltage >= 0.0f);
    TEST_ASSERT_TRUE(voltage <= 5.0f);
}

void test_millivolts_should_return_battery_voltage_in_millivolts(void) {
    uint16_t millivolts = battery.millivolts();
    TEST_ASSERT_TRUE(millivolts >= 0);
    TEST_ASSERT_TRUE(millivolts <= 5000);
}

void test_remaining_should_return_battery_percentage(void) {
    int8_t remaining = battery.remaining();
    TEST_ASSERT_TRUE(remaining >= 0);
    TEST_ASSERT_TRUE(remaining <= 100);
}

void test_system_clock_should_initialize(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    TEST_ASSERT_TRUE(systemTime.getUnixTime() > 0);
    TEST_ASSERT_TRUE(systemTime.getUnixTimeUsec() > 0);
    TEST_ASSERT_TRUE(systemTime.getUnixTimeNsec() > 0);
}

// The clock cases below WRITE the real DS1307, the same way check_clock.py does
// over the link. Each one restores what it found, so a full pass leaves the board
// on the time it started with; a case that fails partway can leave it up to an
// hour behind, which a ground station corrects on the next contact.

void test_subsecond_part_is_present_and_never_goes_backwards(void) {
    TEST_ASSERT_TRUE(systemTime.begin());

    bool sawFraction = false;
    uint64_t previous = systemTime.getUnixTimeUsec();
    TEST_ASSERT_TRUE(previous > 0);

    // Long enough to cross several seconds boundaries, which is where a read
    // without the carry retry would pair a stale second with a fresh fraction
    // and step backwards.
    for (uint32_t i = 0; i < 20000; ++i) {
        uint64_t sample = systemTime.getUnixTimeUsec();
        TEST_ASSERT_TRUE(sample >= previous);
        if ((sample % USEC_PER_SEC) != 0) {
            sawFraction = true;
        }
        previous = sample;
    }

    TEST_ASSERT_TRUE_MESSAGE(sawFraction, "every reading landed exactly on a second");
}

void test_plausibility_floor(void) {
    TEST_ASSERT_FALSE(SystemTime::isPlausible(0));
    TEST_ASSERT_FALSE(SystemTime::isPlausible(1640995199));  // 2021-12-31T23:59:59Z
    TEST_ASSERT_TRUE(SystemTime::isPlausible(1640995200));   // 2022-01-01T00:00:00Z
}

void test_set_unix_time_reports_whether_it_accepted(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    time_t original = systemTime.getUnixTime();
    TEST_ASSERT_TRUE(SystemTime::isPlausible(original));

    TEST_ASSERT_FALSE(systemTime.setUnixTime(0, SystemTime::Source::Ground));
    TEST_ASSERT_FALSE(systemTime.setUnixTime(1640995199, SystemTime::Source::Ground));
    TEST_ASSERT_EQUAL_INT32(original, systemTime.getUnixTime());

    TEST_ASSERT_TRUE(systemTime.setUnixTime(original, SystemTime::Source::Ground));
    TEST_ASSERT_EQUAL(SystemTime::Source::Ground, systemTime.source());

    // Ground outranks Ds1307, so a re-seed must now be refused -- this is what
    // stops the periodic re-seed from undoing a time the ground set.
    TEST_ASSERT_FALSE(systemTime.reseedFromDs1307());
    TEST_ASSERT_EQUAL(SystemTime::Source::Ground, systemTime.source());
}

void test_time_since_boot_survives_a_backwards_clock_set(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    time_t original = systemTime.getUnixTime();

    uint64_t before = systemTime.sinceBootUsec();
    TEST_ASSERT_TRUE(systemTime.setUnixTime(original - 3600, SystemTime::Source::Ground));
    uint64_t after = systemTime.sinceBootUsec();

    // The wall clock went back an hour; elapsed time must not have.
    TEST_ASSERT_TRUE_MESSAGE(after >= before, "time since boot went backwards");
    TEST_ASSERT_TRUE_MESSAGE(after - before < 60ULL * USEC_PER_SEC,
                             "time since boot absorbed the correction");
    TEST_ASSERT_EQUAL_INT32(original - 3600, systemTime.getUnixTime());

    systemTime.setUnixTime(original, SystemTime::Source::Ground);
}

void test_an_accepted_time_reaches_the_ds1307(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    time_t original = systemTime.getUnixTime();

    TEST_ASSERT_TRUE(systemTime.setUnixTime(original - 3600, SystemTime::Source::Ground));

    // A fresh object seeds itself from the DS1307, so what it reads back is what
    // the write-through put there. This is the propagation that lets Ds1307
    // outrank Survived in the ladder.
    SystemTime reopened;
    TEST_ASSERT_TRUE(reopened.begin());
    TEST_ASSERT_EQUAL(SystemTime::Source::Ds1307, reopened.source());
    TEST_ASSERT_INT32_WITHIN(2, original - 3600, reopened.getUnixTime());

    reopened.setUnixTime(original, SystemTime::Source::Ground);
}

// pushToDs1307() is the half of the periodic reconciliation that runs under a
// ground-set clock, and in firmware it only fires every few hours, so this is its
// only coverage. It exists because correcting a drifted DS1307 was moved off the
// inbound-message path, where it cost an I2C round trip per SYSTEM_TIME.
void test_push_to_ds1307_corrects_a_drifted_external_clock(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    time_t original = systemTime.getUnixTime();
    TEST_ASSERT_TRUE(SystemTime::isPlausible(original));

    // Ground is the authority, and the DS1307 is then dragged out of agreement
    // behind the library's back -- which is what drift looks like from here.
    TEST_ASSERT_TRUE(systemTime.setUnixTime(original, SystemTime::Source::Ground));
    RTC_DS1307 ds1307;
    TEST_ASSERT_TRUE(ds1307.begin());
    ds1307.adjust(DateTime((uint32_t)(original - 7200)));
    TEST_ASSERT_INT32_WITHIN(2, original - 7200, (time_t)ds1307.now().unixtime());

    TEST_ASSERT_TRUE(systemTime.pushToDs1307());
    TEST_ASSERT_INT32_WITHIN(2, systemTime.getUnixTime(), (time_t)ds1307.now().unixtime());

    // Nothing left to correct, so it reports that it did nothing.
    TEST_ASSERT_FALSE(systemTime.pushToDs1307());
}

// Reports R64CNT's observed range, so the 7-bit / 128-counts-per-second reading
// the sub-second fraction depends on is evidenced rather than deduced from the
// register's misleading name. A first attempt masked six bits, which made the
// fraction wrap twice a second; this is what would have shown that immediately.
void test_report_r64cnt_range(void) {
    // R64CNT only advances once the RTC has been opened, and setUp() rebuilds the
    // object without calling begin(). Relying on an earlier case having opened it
    // makes this one order-dependent, so it opens it itself.
    TEST_ASSERT_TRUE(systemTime.begin());

    uint8_t lowest = 0xFF;
    uint8_t highest = 0;
    uint32_t transitions = 0;
    uint8_t last = (uint8_t)(R_RTC->R64CNT & 0x7F);

    // Two whole seconds, so a counter that spans 0..127 once per second is
    // distinguishable from one that spans 0..63 twice.
    uint32_t started = millis();
    while (millis() - started < 2000) {
        uint8_t now = (uint8_t)(R_RTC->R64CNT & 0x7F);
        if (now < lowest) lowest = now;
        if (now > highest) highest = now;
        if (now != last) {
            transitions++;
            last = now;
        }
    }

    char message[96];
    snprintf(message, sizeof(message),
             "R64CNT over 2 s: lowest=%u highest=%u transitions=%lu",
             (unsigned)lowest, (unsigned)highest, (unsigned long)transitions);
    TEST_MESSAGE(message);

    // 128 counts a second means ~256 transitions in two seconds and a top of
    // 127; 64 counts a second would top out at 63.
    TEST_ASSERT_TRUE_MESSAGE(highest > 63, "R64CNT never exceeded 63: not a 7-bit count");
    TEST_ASSERT_TRUE_MESSAGE(transitions > 200 && transitions < 320,
                             "R64CNT did not advance at ~128 Hz");
}

// Task 1.3 of improve-clock-synchronisation: no path on the link exposes the two
// clocks separately, so the drift measurement lives here. It asserts nothing about
// the figure -- it reports it, and is meant to be run twice at least an hour apart
// so the difference between the two reports is the drift. The DS1307 is read
// directly rather than through SystemTime: the ownership rule that keeps it behind
// lib/SystemTime exists to avoid two tasks reaching one peripheral, and this binary
// has no tasks at all.
void test_report_internal_versus_ds1307_drift(void) {
    TEST_ASSERT_TRUE(systemTime.begin());

    RTC_DS1307 ds1307;
    TEST_ASSERT_TRUE_MESSAGE(ds1307.begin(), "no DS1307 to compare against");

    // begin() has just seeded the internal clock from the DS1307, so let them run
    // apart for a moment before reading, or the difference is always zero.
    delay(2000);

    time_t internal = systemTime.getUnixTime();
    time_t external = (time_t)ds1307.now().unixtime();

    char message[96];
    snprintf(message, sizeof(message),
             "drift: internal=%ld ds1307=%ld difference=%ld s",
             (long)internal, (long)external, (long)(internal - external));
    TEST_MESSAGE(message);
}

void test_sddata_write_and_rotate(void) {
    // SdData is format-agnostic now: it takes bytes, and src/sdwrite.cpp is what
    // knows they are DataFlash records. This exercises the ring, not the format,
    // so an arbitrary fixed-size payload is the honest thing to write here.
    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }
    // One outer iteration is one file's worth at the new size, so this still crosses
    // every file of the ring and then some.
    for (int i = 0; i < TEST_FILE_COUNT + 2; ++i) {
        for (int j = 0; j < (int)(TEST_FILE_SIZE_BYTES / sizeof(payload)); ++j) {
          sdData.write(payload, sizeof(payload));
        }
    }

    int fileCount = 0;
    for (int i = 0; i < TEST_FILE_COUNT; ++i) {
        String fname = "data" + String(i) + ".BIN";
        if (SD.exists(fname.c_str())) {
            fileCount++;
        }
    }
    TEST_ASSERT_EQUAL(TEST_FILE_COUNT, fileCount);
    TEST_ASSERT_TRUE(SD.exists("index.bin"));
}

void test_set_unix_time_distinguishes_accepted_from_moved(void) {
    TEST_ASSERT_TRUE(systemTime.begin());
    time_t original = systemTime.getUnixTime();
    TEST_ASSERT_TRUE(SystemTime::isPlausible(original));

    // Accepted AND moved: a different plausible second.
    bool moved = false;
    TEST_ASSERT_TRUE(systemTime.setUnixTime(original + 120, SystemTime::Source::Ground, &moved));
    TEST_ASSERT_TRUE_MESSAGE(moved, "a real clock change must report clockMoved");

    // Accepted and NOT moved: the second the clock already holds. This is the case the
    // reference GCS generates once a second, and reading the return value as "changed"
    // is what made the flight log append a TIME record per second.
    //
    // Retried rather than asserted once: if a second boundary falls between reading the
    // held value and offering it back, setUnixTime sees a DIFFERENT second and correctly
    // writes the clock, which would fail this spuriously. Eight attempts all landing on
    // a boundary is not a real possibility.
    bool sawNotMoved = false;
    for (int attempt = 0; attempt < 8 && !sawNotMoved; ++attempt) {
        time_t held = systemTime.getUnixTime();
        moved = true;
        TEST_ASSERT_TRUE(systemTime.setUnixTime(held, SystemTime::Source::Ground, &moved));
        if (!moved) {
            sawNotMoved = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sawNotMoved, "an equal second must report clockMoved false");

    // Refused for implausibility: not moved either.
    moved = true;
    TEST_ASSERT_FALSE(systemTime.setUnixTime(0, SystemTime::Source::Ground, &moved));
    TEST_ASSERT_FALSE_MESSAGE(moved, "a refused time must not report clockMoved");

    // Refused as a demotion: Ground is held, so a Ds1307 re-seed cannot move it.
    moved = true;
    TEST_ASSERT_FALSE(systemTime.reseedFromDs1307(&moved));
    TEST_ASSERT_FALSE_MESSAGE(moved, "a refused re-seed must not report clockMoved");

    systemTime.setUnixTime(original, SystemTime::Source::Ground);
}

// State for the onOpen coverage below. File-scope rather than captured, because
// SdDataOnOpen is a plain function pointer -- deliberately, so registering a callback
// allocates nothing.
static int onOpenCalls = 0;
static bool onOpenInside = false;
static bool onOpenReentered = false;

static void countingOnOpen(SdData &sd) {
    // If the callback is ever entered while already inside itself, the writeRaw/write
    // split has failed and rotation has re-entered itself. Recorded rather than
    // asserted here: Unity assertions from inside a callback would unwind through
    // library code.
    if (onOpenInside) {
        onOpenReentered = true;
        return;
    }
    onOpenInside = true;
    onOpenCalls++;

    // Deliberately through writeRaw, exactly as src/sdwrite.cpp writes the preamble.
    // writeRaw does not check the size limit, which is what makes this safe.
    uint8_t marker[8] = {0xA3, 0x95, 128, 0, 0, 0, 0, 0};
    sd.writeRaw(marker, sizeof(marker));

    onOpenInside = false;
}

void test_sddata_on_open_fires_on_rotation_without_re_entering(void) {
    onOpenCalls = 0;
    onOpenInside = false;
    onOpenReentered = false;

    sdData.setOnOpen(countingOnOpen);

    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }
    // Enough to cross every file of the ring several times.
    for (int i = 0; i < TEST_FILE_COUNT + 2; ++i) {
        for (int j = 0; j < (int)(TEST_FILE_SIZE_BYTES / sizeof(payload)); ++j) {
            sdData.write(payload, sizeof(payload));
        }
    }

    sdData.setOnOpen(nullptr);

    // Fired at least once, so rotation really does notify.
    TEST_ASSERT_TRUE_MESSAGE(onOpenCalls > 0, "onOpen never fired on rotation");
    // Never re-entered: the callback writes through writeRaw, which cannot rotate.
    TEST_ASSERT_FALSE_MESSAGE(onOpenReentered, "onOpen re-entered itself: writeRaw rotated");
    // Bounded. Runaway recursion would either overflow the stack or drive this far
    // past the number of rotations 600 writes of 32 B into 1024 B files can cause.
    TEST_ASSERT_TRUE_MESSAGE(onOpenCalls < 100, "onOpen fired implausibly often");

    // The ring is still intact after all that.
    int fileCount = 0;
    for (int i = 0; i < TEST_FILE_COUNT; ++i) {
        String fname = "data" + String(i) + ".BIN";
        if (SD.exists(fname.c_str())) {
            fileCount++;
        }
    }
    TEST_ASSERT_EQUAL(TEST_FILE_COUNT, fileCount);

    // The other open that fires this callback -- the one inside begin() -- is covered
    // by test_sddata_on_open_fires_on_begin below. It was unreachable from here until
    // begin() became idempotent.
}

// Reads a file's length without disturbing whatever else holds it. Returns 0 for a
// file that does not exist, which is distinguishable here because every file these
// tests care about has had something written to it.
static uint32_t fileSizeOf(const char *name) {
    if (!SD.exists(name)) {
        return 0;
    }
    File f = SD.open(name, FILE_READ);
    if (!f) {
        return 0;
    }
    uint32_t size = f.size();
    f.close();
    return size;
}

// Writes index.bin the way SdData::writeLogIndex() does -- a bare int at offset 0 --
// so readLogIndex() will believe it. This is how a test moves the ring's position out
// from under the object.
static void writeLogIndexByHand(int idx) {
    if (SD.exists("index.bin")) {
        SD.remove("index.bin");
    }
    File f = SD.open("index.bin", FILE_WRITE);
    if (!f) {
        return;
    }
    f.seek(0);
    f.write(reinterpret_cast<uint8_t *>(&idx), sizeof(idx));
    f.flush();
    f.close();
}

void test_sddata_on_open_fires_on_begin(void) {
    onOpenCalls = 0;
    onOpenInside = false;
    onOpenReentered = false;

    sdData.setOnOpen(countingOnOpen);

    // First, with a file already open: setUp() called begin() before this body ran, so
    // this is exactly the call that used to return early and fire nothing.
    sdData.begin();
    // Then from a closed state, which also exercises end().
    sdData.end();
    sdData.begin();

    sdData.setOnOpen(nullptr);

    TEST_ASSERT_EQUAL_MESSAGE(2, onOpenCalls,
                              "begin() must fire onOpen on every open, held file or not");
    TEST_ASSERT_FALSE_MESSAGE(onOpenReentered, "onOpen re-entered itself from begin()");

    // What this does NOT prove: that the bytes the callback wrote are in the file a
    // reader will later open. cleanSdFiles() deletes those files on every case and this
    // suite has no DataFlash reader, so it can only show that the callback ran. Reading
    // the preamble back off a real card is a separate, hands-on step.
}

void test_sddata_begin_reopens_the_file_the_index_names(void) {
    sdData.setOnOpen(nullptr);

    // setUp() cleaned the card and called begin(), and cleanSdFiles() removed
    // index.bin, so readLogIndex() returned 0 and the object holds data0.BIN.
    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }
    sdData.write(payload, sizeof(payload));

    // Move the ring's position while a file is open, which is what a card remount or an
    // error-recovery restart would do.
    writeLogIndexByHand(2);
    sdData.begin();

    // Sizes are read only after begin() has closed data0.BIN, so nothing here opens a
    // file the object is holding.
    uint32_t zeroAfterBegin = fileSizeOf("data0.BIN");
    TEST_ASSERT_TRUE_MESSAGE(zeroAfterBegin > 0, "setUp's begin() never wrote to data0.BIN");

    sdData.write(payload, sizeof(payload));

    // Closed before measuring, so nothing below opens a file this object still holds --
    // two SdFile instances on one file share the single SdVolume cache block, and there
    // is no reason to lean on that here.
    sdData.end();

    // Against the old begin() this fails on the first assertion: it returned early
    // because a file was open, data2.BIN was never created, and the write went to
    // data0.BIN instead.
    TEST_ASSERT_TRUE_MESSAGE(SD.exists("data2.BIN"),
                             "begin() did not open the file index.bin names");
    TEST_ASSERT_TRUE_MESSAGE(fileSizeOf("data2.BIN") > 0,
                             "writes did not follow begin() to the new file");
    TEST_ASSERT_EQUAL_MESSAGE(zeroAfterBegin, fileSizeOf("data0.BIN"),
                              "writes still went to the file begin() left behind");
}

// write() accumulates and syncs once per FLUSH_INTERVAL_BYTES instead of syncing every
// record. What a reader sees is the DIRECTORY ENTRY, which only a sync updates, so the
// observable consequence is that a file reports less than what has been handed to it
// until the interval is reached. That is also exactly the bound the spec declares on what
// power loss costs, which is why it is worth a case of its own.
//
// Note that reading the size is not free of side effects: SD.open() for reading walks the
// directory, which evicts the dirty data block and therefore writes it. The DATA reaches
// the card; the recorded size does not. That is the distinction being asserted.
void test_sddata_write_batches_its_flushes(void) {
    sdData.setOnOpen(nullptr);

    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    // Under the interval.
    const uint32_t under = 2048;
    for (uint32_t n = 0; n < under; n += sizeof(payload)) {
        sdData.write(payload, sizeof(payload));
    }
    uint32_t visibleUnder = fileSizeOf("data0.BIN");

    // Past it.
    const uint32_t past = 5120;
    for (uint32_t n = under; n < past; n += sizeof(payload)) {
        sdData.write(payload, sizeof(payload));
    }
    uint32_t visiblePast = fileSizeOf("data0.BIN");

    // Against the old per-record flush the first assertion fails: every record synced, so
    // the recorded size kept pace with what was written and visibleUnder was 2048.
    TEST_ASSERT_TRUE_MESSAGE(visibleUnder < under,
                             "write() synced before reaching its interval");
    TEST_ASSERT_TRUE_MESSAGE(visiblePast >= 4096,
                             "write() did not sync on reaching its interval");
}

// A rotation must not lose what has been accumulated but not yet synced. close() syncs,
// so this passes by construction today -- but "by construction" is the claim being
// checked, and it is the one thing about batching that could silently drop data.
//
// This is NOT a test that the byte counter is reset at a rotation. That was the original
// intent and it turned out to be untestable, because carrying the counter across a
// rotation would only make the new file's first interval SHORTER -- a smaller bound, not a
// violated one, and nothing observable. The reset is still done, and is still right, but
// the thing worth asserting here is that the tail survives.
void test_sddata_rotation_does_not_lose_unsynced_bytes(void) {
    sdData.setOnOpen(nullptr);

    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    // Exactly one file's worth, so data0.BIN fills and rotation closes it.
    for (uint32_t n = 0; n < TEST_FILE_SIZE_BYTES; n += sizeof(payload)) {
        sdData.write(payload, sizeof(payload));
    }

    // The closed file must report everything written to it, not merely everything that
    // had been synced when the last interval elapsed.
    uint32_t closed = fileSizeOf("data0.BIN");
    TEST_ASSERT_EQUAL_MESSAGE(TEST_FILE_SIZE_BYTES, closed,
                              "rotation lost the bytes written since the last sync");
    TEST_ASSERT_TRUE_MESSAGE(SD.exists("data1.BIN"), "rotation did not open the next file");
}

// Reports the FAT geometry of whatever card is in the board, because one figure in it
// -- the cluster size -- decides whether a log rotation fits inside WDT_TIMEOUT_MS, and
// nothing in the firmware or the tests had ever read it. Same shape as
// test_report_r64cnt_range above: report the numbers, assert only what the design needs
// to be true of them.
//
// Why rotation cares. SdVolume::freeChain() walks a deleted file's cluster chain one
// cluster at a time, fatGet() then fatPut() each, and fatPut() dirties the mirror FAT as
// well. So SD.remove() costs roughly one sector read and fatCount() sector writes per
// FAT sector the chain spans, and that is proportional to the FILE SIZE. At the shipped
// 1 GiB it can be thousands of operations; at a megabyte it is a handful. The figures
// below are what turns that from an argument into a number.
//
// This builds its OWN Sd2Card and SdVolume, because SDClass keeps both private and
// befriends only File. That is not merely rude -- doing it naively corrupts the rest of
// the suite -- so the shape below is deliberate in three ways:
//
//   * SdVolume::sdCard_ is STATIC (SdFat.h), and SdVolume::init() assigns it. So
//     vol.init(&probeCard) repoints the card pointer that the SdVolume INSIDE SD uses as
//     well, and it keeps pointing there until something reassigns it. The probe card
//     therefore has STATIC STORAGE: a local would be destroyed on return and every later
//     SD access would dereference a dead stack object -- and write to the card through it.
//     Static storage makes that impossible to get wrong, including on the paths below
//     where the restore does not happen.
//   * SD.begin() puts the pointer back where it belongs -- SDClass::begin() re-runs
//     card.init(), volume.init() and openRoot() on its own members -- but it can FAIL, and
//     the assertion reporting that failure longjmps straight into tearDown(). That is why
//     the pointer must be left somewhere valid rather than merely restored: on the failure
//     path cleanSdFiles() runs through probeCard, which is alive and initialised on the
//     same CS, instead of through a corpse.
//   * Every assertion is still deferred until after the restore attempt, so the ordinary
//     failure of a probe cannot skip it.
//   * SdVolume's cache members are static too, so this evicts whatever the log had
//     cached. Harmless -- it goes through cacheRawBlock(), which flushes first -- and
//     sdData.end() closes the log file so no handle is open across Sd2Card::init()'s
//     card reset. setUp() reopens on the next case.
void test_report_sd_volume_geometry(void) {
    sdData.end();

    // Static, not local: see the third point above. Costs sizeof(Sd2Card) in the test
    // binary's .bss and nothing at all in the flight image.
    static Sd2Card probeCard;

    bool cardOk = probeCard.init(SPI_HALF_SPEED, 9);
    bool volumeOk = false;
    uint8_t fatType = 0;
    uint8_t blocksPerCluster = 0;
    uint8_t fatCount = 0;
    uint32_t clusterCount = 0;
    uint32_t blocksPerFat = 0;
    uint32_t cardBlocks = 0;

    if (cardOk) {
        SdVolume vol;
        volumeOk = vol.init(&probeCard);
        if (volumeOk) {
            fatType = vol.fatType();
            blocksPerCluster = vol.blocksPerCluster();
            fatCount = vol.fatCount();
            clusterCount = vol.clusterCount();
            blocksPerFat = vol.blocksPerFat();
            cardBlocks = probeCard.cardSize();
        }
    }

    // vol is gone, which is safe -- nothing stores a pointer to an SdVolume. probeCard is
    // not gone, which is the point.
    bool restored = SD.begin(9);

    TEST_ASSERT_TRUE_MESSAGE(restored, "could not restore SD's own card/volume association");
    TEST_ASSERT_TRUE_MESSAGE(cardOk, "Sd2Card::init failed on CS 9");
    TEST_ASSERT_TRUE_MESSAGE(volumeOk, "SdVolume::init failed: not a FAT volume?");

    uint32_t clusterBytes = (uint32_t)blocksPerCluster * 512UL;
    uint32_t entriesPerFatSector = 512UL / (fatType == 32 ? 4UL : 2UL);

    // Asserted HERE, before the arithmetic below divides by clusterBytes, and not at the
    // end with the rest. A mounted FAT volume cannot have a cluster smaller than one
    // block and blocksPerCluster is a power of two by the format's own definition, so
    // these hold or the volume was not really read -- which is the failure a report-only
    // case would otherwise hide. Putting them after the division would mean the guard
    // runs only if the thing it guards against did not happen.
    TEST_ASSERT_TRUE_MESSAGE(fatType == 16 || fatType == 32, "unexpected FAT type");
    TEST_ASSERT_TRUE_MESSAGE(clusterBytes >= 512UL, "cluster smaller than a block");
    TEST_ASSERT_EQUAL_MESSAGE(0, blocksPerCluster & (blocksPerCluster - 1),
                              "blocksPerCluster is not a power of two");
    TEST_ASSERT_TRUE_MESSAGE(clusterCount > 0, "volume reports no clusters");
    TEST_ASSERT_TRUE_MESSAGE(cardBlocks > 0, "card reports zero size");

    char message[128];
    snprintf(message, sizeof(message),
             "FAT%u: cluster=%lu B (%u blocks) clusters=%lu fatBlocks=%lu fats=%u",
             (unsigned)fatType, (unsigned long)clusterBytes,
             (unsigned)blocksPerCluster, (unsigned long)clusterCount,
             (unsigned long)blocksPerFat, (unsigned)fatCount);
    TEST_MESSAGE(message);

    snprintf(message, sizeof(message), "card=%lu blocks (%lu MiB)",
             (unsigned long)cardBlocks, (unsigned long)(cardBlocks / 2048UL));
    TEST_MESSAGE(message);

    // What deleting one file costs at the shipped size and at the proposed one, reported
    // for both so the comparison does not have to be done by hand later.
    //
    // These are CONTIGUOUS-CHAIN estimates and the worst case is reported beside them.
    // freeChain() follows the chain wherever it leads, so a fragmented file can visit a
    // separate FAT sector per cluster; the floor assumes consecutive entries sharing a
    // sector, the ceiling assumes none do. A log file written straight through in one
    // pass is near the floor, but nothing enforces that.
    const uint32_t sizes[] = {1024UL * 1024UL * 1024UL, 1024UL * 1024UL};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t clusters = (sizes[i] + clusterBytes - 1UL) / clusterBytes;
        uint32_t fatSectors = (clusters + entriesPerFatSector - 1UL) / entriesPerFatSector;
        uint32_t best = fatSectors * (1UL + (uint32_t)fatCount);
        uint32_t worst = clusters * (1UL + (uint32_t)fatCount);
        snprintf(message, sizeof(message),
                 "deleting %lu KiB: %lu clusters, %lu FAT sectors, ~%lu ops contiguous, "
                 "<=%lu fragmented",
                 (unsigned long)(sizes[i] / 1024UL), (unsigned long)clusters,
                 (unsigned long)fatSectors, (unsigned long)best, (unsigned long)worst);
        TEST_MESSAGE(message);
    }

}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voltaje_should_return_battery_voltage);
    RUN_TEST(test_millivolts_should_return_battery_voltage_in_millivolts);
    RUN_TEST(test_remaining_should_return_battery_percentage);
    RUN_TEST(test_system_clock_should_initialize);
    RUN_TEST(test_subsecond_part_is_present_and_never_goes_backwards);
    RUN_TEST(test_plausibility_floor);
    RUN_TEST(test_set_unix_time_reports_whether_it_accepted);
    RUN_TEST(test_set_unix_time_distinguishes_accepted_from_moved);
    RUN_TEST(test_time_since_boot_survives_a_backwards_clock_set);
    RUN_TEST(test_an_accepted_time_reaches_the_ds1307);
    RUN_TEST(test_push_to_ds1307_corrects_a_drifted_external_clock);
    RUN_TEST(test_report_r64cnt_range);
    RUN_TEST(test_report_internal_versus_ds1307_drift);
    RUN_TEST(test_sddata_write_and_rotate);
    RUN_TEST(test_sddata_on_open_fires_on_rotation_without_re_entering);
    RUN_TEST(test_sddata_on_open_fires_on_begin);
    RUN_TEST(test_sddata_begin_reopens_the_file_the_index_names);
    RUN_TEST(test_sddata_write_batches_its_flushes);
    RUN_TEST(test_sddata_rotation_does_not_lose_unsynced_bytes);
    RUN_TEST(test_report_sd_volume_geometry);
    return UNITY_END();
}

// The linker needs this; nothing here can ever call it.
//
// This environment sets test_build_src = no, so src/hooks.cpp -- which defines the
// real hook -- is not in the test binary. It became necessary when lib/SystemTime
// started calling vTaskSuspendAll() to make setUnixTime()'s clock-and-epoch update
// indivisible: that pulls FreeRTOS's tasks.c into the link, and tasks.c references
// this hook because configCHECK_FOR_STACK_OVERFLOW is 2.
//
// A stub is honest rather than a shortcut. This suite never starts a scheduler --
// setup() calls runUnityTests() directly and creates no task -- so
// vTaskSwitchContext, the only caller, never runs. Do NOT read a passing run as
// evidence that overflow detection works: that lives in src/, which this environment
// deliberately excludes.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    (void)pcTaskName;
}

/**
  * For Arduino framework
  */
void setup() {
  // Wait ~2 seconds before the Unity test runner
  // establishes connection with a board Serial interface
  delay(2000);
  SD.begin(9);
  runUnityTests();
}
void loop() {}
