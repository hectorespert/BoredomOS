#ifndef BOREDOMOS_SYSTEM_H
#define BOREDOMOS_SYSTEM_H

struct System {
    size_t heap;
    UBaseType_t taskLoggerAvailableStack;
    UBaseType_t taskHeartbeatAvailableStack;
    UBaseType_t taskStatusAvailableStack;
    UBaseType_t taskSdWriteAvailableStack;
    UBaseType_t taskMavlinkAvailableStack;
    UBaseType_t taskSerialReadAvailableStack;
    UBaseType_t taskSerialWriteAvailableStack;
};

#endif //BOREDOMOS_SYSTEM_H