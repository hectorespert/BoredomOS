#ifndef BOREDOMOS_DATA_H
#define BOREDOMOS_DATA_H
#include <Arduino_FreeRTOS.h>

struct Tasks
{
    UBaseType_t loggerAvailableStack;
    UBaseType_t heartbeatAvailableStack;
    UBaseType_t statusAvailableStack;
    UBaseType_t sdWriteAvailableStack;
    UBaseType_t mavlinkAvailableStack;
    UBaseType_t serialReadAvailableStack;
    UBaseType_t serialWriteAvailableStack;
};

struct System {
    size_t heap;
    Tasks tasks;
};

struct Energy {
    uint16_t millivolts;
    int8_t remaining;
};

struct Data
{
    uint32_t unixtime;
    uint32_t uptime;
    System system;
    Energy energy;
};


#endif //BOREDOMOS_DATA_H
