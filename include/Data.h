#ifndef BOREDOMOS_DATA_H
#define BOREDOMOS_DATA_H
#include <Arduino_FreeRTOS.h>

// Seven per-task marks, not five: replace-console-cli-with-usb-mavlink-link
// split the link tasks per port and deleted the CLI (which was never recorded
// here). Measured with a compile-time probe rather than counted by hand:
// sizeof(Tasks) is 28 B and sizeof(Data) goes from 36 B to 44 B. That
// re-derives sdWriteQueue's heap backing -- see ARCHITECTURE.md section 4,
// whose table claimed 56 B and had been wrong before this change too.
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
