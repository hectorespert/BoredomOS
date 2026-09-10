#include <unity.h>
#include <Arduino.h>
#include <Battery.h>
#include <SystemTime.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <SdData.h>

#define TEST_FILE_COUNT 4
#define TEST_FILE_SIZE_MB 1024UL

Battery battery;
SystemTime systemTime;
SdData sdData(TEST_FILE_COUNT, TEST_FILE_SIZE_MB);

void cleanSdFiles() {
    for (int i = 0; i < TEST_FILE_COUNT; ++i) {
        String fname = "data" + String(i) + ".mpk";
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

void test_sddata_write_and_rotate(void) {
    StaticJsonDocument<128> doc;
    for (int i = 0; i < TEST_FILE_COUNT + 2; ++i) {
        for (int j = 0; j < 100; ++j) {
          doc["test"] = j;
          sdData.write(doc);
        }
    }

    int fileCount = 0;
    for (int i = 0; i < TEST_FILE_COUNT; ++i) {
        String fname = "data" + String(i) + ".mpk";
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
    RUN_TEST(test_sddata_write_and_rotate);
    return UNITY_END();
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
