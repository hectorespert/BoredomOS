#ifndef BATTERY_H
#define BATTERY_H
#include <SolarCharger.h>
#include <stdint.h>

class Battery
{
public:
  Battery();
  float voltage();
  uint16_t millivolts();
  int8_t remaining();
private:
    SolarCharger _solarCharger;
    float _cachedVoltage = 0.0f;
    unsigned long _lastRead = 0;
};

#endif