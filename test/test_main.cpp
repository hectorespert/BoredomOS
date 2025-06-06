#include <unity.h>
#include <Arduino.h>
#include <Battery.h>
#include <SystemTime.h>

Battery battery;
SystemTime systemTime;

void setUp(void) {
    battery = Battery();
    systemTime = SystemTime();
}

void tearDown(void) {
  // clean stuff up here
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

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voltaje_should_return_battery_voltage);
    RUN_TEST(test_millivolts_should_return_battery_voltage_in_millivolts);
    RUN_TEST(test_remaining_should_return_battery_percentage);
    RUN_TEST(test_system_clock_should_initialize);
    return UNITY_END();
}

/**
  * For Arduino framework
  */
void setup() {
  // Wait ~2 seconds before the Unity test runner
  // establishes connection with a board Serial interface
  delay(2000);

  runUnityTests();
}
void loop() {}
