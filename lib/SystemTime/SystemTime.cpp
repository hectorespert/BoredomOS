#include <SystemTime.h>
#include <Arduino.h>
#include <RTC.h>

SystemTime::SystemTime()
{
    _ds1307 = RTC_DS1307();
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
    return getUnixTime() * 1000000ULL;
}

int64_t SystemTime::getUnixTimeNsec()
{
    return getUnixTime() * 1000000000ULL;
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