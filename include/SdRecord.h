#ifndef BOREDOMOS_SD_RECORD_H
#define BOREDOMOS_SD_RECORD_H

#include <Arduino_FreeRTOS.h>
#include <stdint.h>

// What travels on sdWriteQueue: what a producer MEANS, not wire-ready bytes.
// src/sdwrite.cpp is the only place that turns one of these into a DataFlash
// record, because it owns the card and therefore owns the log's format. This
// mirrors include/LinkMsg.h, where a producer posts an intent and the file that
// owns the protocol does the packing (ARCHITECTURE.md section 4).
//
// Carried BY VALUE, which is forced rather than preferred: the battery record
// must come from TaskMavlink -- the task that already reads lib/Battery, so that
// Battery's unguarded 125 ms cache never becomes state shared across two
// priorities -- and CI forbids src/mavlink.cpp from calling pvPortMalloc. A
// heap-pointer producer in that file is therefore not available. Nothing in the
// firmware calls pvPortMalloc afterwards.

enum class SdRecordKind : uint8_t
{
    Sys = 0,   // free heap and every task's stack headroom, 1 Hz
    Pwr = 1,   // battery, at the rate the battery is actually read
    Time = 2,  // wall clock and its origin, on file open and on a clock set
    // The log protocol's requests, forwarded by TaskMavlink to the card's owner
    // (download-the-flight-log). They are not log records: TaskSdWrite answers them
    // on the requesting port's write queue and writes nothing to the card.
    LogList = 3,
    LogRead = 4,
    LogEnd = 5,
};

// Seven per-task stack marks, in the order SYS's labels declare them:
// Log, SdW, Mav, SRd, SWr, URd, UWr.
constexpr uint8_t SD_RECORD_TASK_COUNT = 7;

struct SdSysPayload
{
    uint64_t timeUs;
    uint16_t heapFree;
    uint16_t stacks[SD_RECORD_TASK_COUNT];
};

struct SdPwrPayload
{
    uint64_t timeUs;
    uint16_t millivolts;
    int8_t remaining;
};

struct SdTimePayload
{
    uint64_t timeUs;
    uint32_t unixtime;
    uint8_t source;  // SystemTime::Source, not a second enumeration
};

struct SdLogRequestPayload
{
    uint32_t ofs;
    uint32_t count;
    uint16_t start;  // LogList: first id; LogRead: the id
    uint16_t end;    // LogList: last id
    uint8_t port;    // the port the request arrived on, and the one the answer leaves by
};

struct SdRecord
{
    SdRecordKind kind;
    union
    {
        SdSysPayload sys;
        SdPwrPayload pwr;
        SdTimePayload time;
        SdLogRequestPayload log;
    };
};

// 32 B, measured with a compile-time probe rather than counted by hand. It is not
// the 24 B of its largest payload because timeUs is a uint64_t: the union takes
// 8-byte alignment, so the tag costs 8 B of the struct. sdWriteQueue's storage in
// src/main.cpp is depth 4 times this, so a change here moves that reservation --
// which is why it is asserted rather than left to drift.
static_assert(sizeof(SdRecord) == 32, "sdWriteQueue's storage in src/main.cpp is sized on this");

#endif // BOREDOMOS_SD_RECORD_H
