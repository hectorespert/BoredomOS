#ifndef BOREDOMOS_DATA_H
#define BOREDOMOS_DATA_H
#include <Arduino_FreeRTOS.h>

// Seven per-task marks, not five: replace-console-cli-with-usb-mavlink-link
// split the link tasks per port and deleted the CLI (which was never recorded
// here). This takes sizeof(Data) from 56 B to 64 B, which re-derives
// sdWriteQueue's heap backing -- see ARCHITECTURE.md section 4.
//
// It also makes a third record shape on a card that may already hold two, and
// the second with seven fields, so the field names rather than the count are
// what tell them apart when reading a .mpk back.
struct Tasks
{
    UBaseType_t loggerAvailableStack;
    UBaseType_t sdWriteAvailableStack;
    UBaseType_t mavlinkAvailableStack;
    UBaseType_t uartReadAvailableStack;
    UBaseType_t uartWriteAvailableStack;
    UBaseType_t usbReadAvailableStack;
    UBaseType_t usbWriteAvailableStack;
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
