#ifndef BOREDOMOS_LOG_H
#define BOREDOMOS_LOG_H
#include <Sensors.h>

typedef struct 
{
    String timestamp;
    uint32_t unixtime;
    Sensors sensors;
} Log;


#endif //BOREDOMOS_LOG_H
