#include <unity.h>
#include <Arduino.h>
#include <Battery.h>

Battery battery;

void setUp(void) {
    battery = Battery();
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

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voltaje_should_return_battery_voltage);
    RUN_TEST(test_millivolts_should_return_battery_voltage_in_millivolts);
    RUN_TEST(test_remaining_should_return_battery_percentage);
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
