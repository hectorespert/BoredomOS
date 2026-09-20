#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SdRecord.h>
#include <SD.h>
#include <SdData.h>
#include <SystemTime.h>

extern QueueHandle_t sdWriteQueue;
extern SystemTime systemTime;

SdData sdData;

// ---------------------------------------------------------------------------
// The log's on-disk format: ArduPilot DataFlash.
//
// This file owns it, because this file owns the card. lib/SdData moves bytes and
// knows nothing about them; no producer forms a record either -- they post an
// SdRecord (include/SdRecord.h) saying what they mean.
//
// Every record opens with 0xA3 0x95, which is a RESYNCHRONISATION marker: a reader
// that meets a truncated record -- the expected way for a CubeSat log to end -- can
// find the next record boundary instead of losing the remainder of the file. That
// property is why this format was chosen over MessagePack, which had no framing at
// all, and it is what the flight-log capability's requirement about a truncated
// record rests on.
//
// The reader is already a dependency: pymavlink's DFReader parses these with
// arbitrary FMT definitions, so nothing project-specific has to exist on the ground.
// ---------------------------------------------------------------------------

static constexpr uint8_t kHead1 = 0xA3;
static constexpr uint8_t kHead2 = 0x95;

static constexpr uint8_t kMsgFmt = 128;
static constexpr uint8_t kMsgTime = 129;
static constexpr uint8_t kMsgSys = 130;
static constexpr uint8_t kMsgPwr = 131;

// DataFlash's own field widths: name is char[4], format char[16], labels char[64].
// Every definition below fits with room to spare -- the longest label string is
// SYS's, at 39 characters.
struct __attribute__((packed)) LogFormat
{
    uint8_t head1;
    uint8_t head2;
    uint8_t msgid;
    uint8_t type;
    uint8_t length;  // on-disk length of the described record, header included
    char name[4];
    char format[16];
    char labels[64];
};

struct __attribute__((packed)) LogTime
{
    uint8_t head1;
    uint8_t head2;
    uint8_t msgid;
    uint64_t timeUs;
    uint32_t unixtime;
    uint8_t source;
};

struct __attribute__((packed)) LogSys
{
    uint8_t head1;
    uint8_t head2;
    uint8_t msgid;
    uint64_t timeUs;
    uint16_t heapFree;
    uint16_t stacks[SD_RECORD_TASK_COUNT];
};

struct __attribute__((packed)) LogPwr
{
    uint8_t head1;
    uint8_t head2;
    uint8_t msgid;
    uint64_t timeUs;
    uint16_t millivolts;
    int8_t remaining;
};

// A declared length that disagrees with the structure actually written produces
// plausible-looking WRONG numbers on the ground rather than a parse error, which is
// the one failure mode of this format that nothing would catch. These cost nothing
// and catch the whole class.
static_assert(sizeof(LogFormat) == 89, "FMT record must be 89 bytes on disk");
static_assert(sizeof(LogTime) == 16, "TIME record must be 16 bytes on disk");
static_assert(sizeof(LogSys) == 27, "SYS record must be 27 bytes on disk");
static_assert(sizeof(LogPwr) == 14, "PWR record must be 14 bytes on disk");

// The preamble is CONSTANT -- format definitions never vary at run time -- so it
// lives in flash and costs no RAM. It is re-emitted on every file open, which is
// what makes each file of the ring readable on its own.
//
// `name` is char[4] with no room for a terminator, so TIME is brace-initialised;
// the three-character names fit a string literal exactly.
static const LogFormat kPreamble[] = {
    {kHead1, kHead2, kMsgFmt, kMsgFmt, sizeof(LogFormat),
     "FMT", "BBnNZ", "Type,Length,Name,Format,Columns"},
    {kHead1, kHead2, kMsgFmt, kMsgTime, sizeof(LogTime),
     {'T', 'I', 'M', 'E'}, "QIB", "TimeUS,Unix,Src"},
    {kHead1, kHead2, kMsgFmt, kMsgSys, sizeof(LogSys),
     "SYS", "QHHHHHHHH", "TimeUS,Heap,Log,SdW,Mav,SRd,SWr,URd,UWr"},
    {kHead1, kHead2, kMsgFmt, kMsgPwr, sizeof(LogPwr),
     "PWR", "QHb", "TimeUS,mV,Pct"},
};

// Called by lib/SdData after it opens a file, at boot and on every rotation.
//
// It writes through writeRaw(), which does not check the size limit and therefore
// cannot rotate -- that split is what stops this callback from re-entering the
// rotation that invoked it.
static void writeLogPreamble(SdData &sd)
{
    sd.writeRaw(reinterpret_cast<const uint8_t *>(kPreamble), sizeof(kPreamble));

    // A wall-clock record, so records in this file can be placed on an absolute
    // timeline without reading any other file. It cannot be part of the constant
    // preamble because it carries a live value, and this task cannot ask TaskMavlink
    // for one: rotation happens inside this task.
    LogTime record = {
        kHead1, kHead2, kMsgTime,
        systemTime.sinceBootUsec(),
        (uint32_t)systemTime.getUnixTime(),
        (uint8_t)systemTime.source(),
    };
    sd.writeRaw(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
}

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    // Registered before begin(), so the first file's open is caught too.
    sdData.setOnOpen(writeLogPreamble);
    sdData.begin();

    for (;;)
    {
        SdRecord incoming;
        if (xQueueReceive(sdWriteQueue, &incoming, portMAX_DELAY) == pdPASS) {
            switch (incoming.kind) {
            case SdRecordKind::Sys: {
                LogSys record = {kHead1, kHead2, kMsgSys,
                                 incoming.sys.timeUs, incoming.sys.heapFree, {0}};
                for (uint8_t i = 0; i < SD_RECORD_TASK_COUNT; i++) {
                    record.stacks[i] = incoming.sys.stacks[i];
                }
                sdData.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
                break;
            }
            case SdRecordKind::Pwr: {
                LogPwr record = {kHead1, kHead2, kMsgPwr,
                                 incoming.pwr.timeUs, incoming.pwr.millivolts,
                                 incoming.pwr.remaining};
                sdData.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
                break;
            }
            case SdRecordKind::Time: {
                LogTime record = {kHead1, kHead2, kMsgTime,
                                  incoming.time.timeUs, incoming.time.unixtime,
                                  incoming.time.source};
                sdData.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
                break;
            }
            }
        }
    }
}
