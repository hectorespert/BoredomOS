#include <SystemTime.h>
#include <Arduino.h>
#include <RTC.h>

SystemTime::SystemTime() : _ds1307()
{
}

bool SystemTime::begin()
{
    if (_ds1307.begin() && RTC.begin()) {
        RTCTime currentTime(_ds1307.now().unixtime());
        RTC.setTime(currentTime);
        return true;
    }
    return false;
}

time_t SystemTime::getUnixTime()
{
    RTCTime currentTime;
    if (RTC.getTime(currentTime)) {
        return currentTime.getUnixTime();
    }
    return 0;
}

uint64_t SystemTime::getUnixTimeUsec()
{
    return getUnixTime() * USEC_PER_SEC;
}

int64_t SystemTime::getUnixTimeNsec()
{
    return getUnixTime() * NSEC_PER_SEC;
}

void SystemTime::setUnixTime(time_t unix_time)
{
    RTCTime currentTime;
    RTC.getTime(currentTime);

    if (currentTime.getUnixTime() == unix_time) {
        return;
    }

    currentTime.setUnixTime(unix_time);
    RTC.setTime(currentTime);

    if (_ds1307.now().unixtime() == currentTime.getUnixTime()) {
        return;
    }

    _ds1307.adjust(DateTime(currentTime.getUnixTime()));
}