#include <Battery.h>
#include <Arduino.h>

Battery::Battery()
    : _solarCharger(A0)
{
    
}

float Battery::voltage()
{
    unsigned long now = millis();
    // Read every 125ms
    if (now - _lastRead > 125) {
        _cachedVoltage = _solarCharger.readVoltage();
        _lastRead = now;
    }
    return _cachedVoltage;
}

uint16_t Battery::millivolts()
{
    return static_cast<uint16_t>(voltage() * 1000.0f);
}

int8_t Battery::remaining()
{
    float voltage = this->voltage();
    float percent = (voltage - 3.5f) / (4.2f - 3.5f) * 100.0f;
    if (percent > 100.0f) percent = 100.0f;
    if (percent < 0.0f) percent = 0.0f;
    return (int8_t)percent;
}
