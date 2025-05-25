#ifndef BOREDOMOS_LOG_H
#define BOREDOMOS_LOG_H
#include <Sensors.h>
#include <System.h>

typedef struct 
{
    String timestamp;
    uint32_t unixtime;
    System system;
    Sensors sensors;
} Log;


#endif //BOREDOMOS_LOG_H
