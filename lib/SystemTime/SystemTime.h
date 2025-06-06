#ifndef SYSTEM_TIME_H
#define SYSTEM_TIME_H
#include <RTClib.h>

class SystemTime
{
public:
  SystemTime();
  bool begin();
  time_t getUnixTime();
  uint64_t getUnixTimeUsec();
  int64_t getUnixTimeNsec();
  void setUnixTime(time_t unix_time);
private:
  RTC_DS1307 _ds1307;
};

#endif