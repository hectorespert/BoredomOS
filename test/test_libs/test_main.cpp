#include <unity.h>
#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <Battery.h>
#include <SystemTime.h>
#include <SD.h>
#include <SdData.h>

#define TEST_FILE_COUNT 4
#define TEST_FILE_SIZE_MB 1024UL

Battery battery;
SystemTime systemTime;
SdData sdData(TEST_FILE_COUNT, TEST_FILE_SIZE_MB);

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
    for (int i = 0; i < TEST_FILE_COUNT + 2; ++i) {
        for (int j = 0; j < 100; ++j) {
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

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voltaje_should_return_battery_voltage);
    RUN_TEST(test_millivolts_should_return_battery_voltage_in_millivolts);
    RUN_TEST(test_remaining_should_return_battery_percentage);
    RUN_TEST(test_system_clock_should_initialize);
    RUN_TEST(test_subsecond_part_is_present_and_never_goes_backwards);
    RUN_TEST(test_plausibility_floor);
    RUN_TEST(test_set_unix_time_reports_whether_it_accepted);
    RUN_TEST(test_time_since_boot_survives_a_backwards_clock_set);
    RUN_TEST(test_an_accepted_time_reaches_the_ds1307);
    RUN_TEST(test_push_to_ds1307_corrects_a_drifted_external_clock);
    RUN_TEST(test_report_r64cnt_range);
    RUN_TEST(test_report_internal_versus_ds1307_drift);
    RUN_TEST(test_sddata_write_and_rotate);
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
