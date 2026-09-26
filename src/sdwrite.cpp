#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <SdRecord.h>
#include <SD.h>
#include <SdData.h>
#include <SystemTime.h>
#include <LinkMsg.h>
#include <LinkPort.h>

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

// ---------------------------------------------------------------------------
// Downloading the log over MAVLink (download-the-flight-log).
//
// This task reads the card for the link because it owns the card: nothing else
// may touch it. TaskMavlink forwards the log protocol's requests on sdWriteQueue,
// and the answers go straight onto the requesting port's write queue as LinkMsgs,
// which src/link.cpp's writers pack and send -- so each port still has one owner.
//
// The log comes first. Every pass drains sdWriteQueue before doing any download
// work, and does at most one piece of it, so a download never delays a record.
// ---------------------------------------------------------------------------

extern LinkPort linkPorts[2];

// A DataFlash TIME record sits right after the four FMT records of the preamble
// every file starts with; its unix time is what LOG_ENTRY reports as time_utc.
static constexpr uint32_t kHeadTimeOffset = sizeof(kPreamble);
static_assert(sizeof(kPreamble) == 356, "the head TIME record is expected at offset 356");

static constexpr uint8_t kLogDataChunk = 90;  // LOG_DATA's payload, and MAVProxy's stride

struct LogListing {
    bool active;
    uint16_t start;
    uint16_t end;
    uint8_t nextSlot;
};

struct LogStream {
    bool active;
    bool absent;       // the id names no file: answer with count 0 and stop
    int8_t slot;
    uint16_t id;
    uint32_t pos;
    uint32_t end;      // exclusive
    bool toFileEnd;    // end is the end of the file, not the end of the requested range
    bool sentAny;
    uint8_t lastCount;
};

static LogListing listings[2];
static LogStream streams[2];

// At most one handle held across passes, bounding newlib's allocation for reading
// to one SdFile. A listing or a new request opens a second one for as long as it
// takes to read a size or 16 bytes, and closes it before returning.
static File reader;
static int8_t readerPort = -1;

static String slotFileName(int slot)
{
    return String("data") + slot + ".BIN";
}

// The port's queue is paced so the log protocol can never hold more than two of
// its slots: the depth in src/main.cpp keeps the other seven for the periodic
// worst case.
static bool portHasRoom(uint8_t port)
{
    return uxQueueMessagesWaiting(linkPorts[port].writeQueue) <= 1;
}

static void closeReader()
{
    if (reader) {
        reader.close();
    }
    readerPort = -1;
}

// The length a download of this slot would deliver now: the synced size, which is
// what a reading handle sees. 0 when the file does not exist.
static uint32_t slotSize(int slot)
{
    String name = slotFileName(slot);
    if (!SD.exists(name.c_str())) {
        return 0;
    }
    File f = SD.open(name.c_str(), FILE_READ);
    if (!f) {
        return 0;
    }
    uint32_t size = f.size();
    f.close();
    return size;
}

static uint32_t slotTimeUtc(int slot)
{
    String name = slotFileName(slot);
    File f = SD.open(name.c_str(), FILE_READ);
    if (!f) {
        return 0;
    }
    LogTime head;
    bool ok = f.seek(kHeadTimeOffset) &&
              f.read(reinterpret_cast<uint8_t *>(&head), sizeof(head)) == (int)sizeof(head);
    f.close();
    // 0 is the field's "not available": the head record is not a TIME record, or it
    // says the board held no wall clock when the file was opened.
    if (!ok || head.head1 != kHead1 || head.head2 != kHead2 || head.msgid != kMsgTime ||
        head.source == (uint8_t)SystemTime::Source::None) {
        return 0;
    }
    return head.unixtime;
}

static void startListing(const SdRecord &request)
{
    LogListing &listing = listings[request.log.port];
    listing.active = true;
    listing.start = request.log.start;
    listing.end = request.log.end;
    listing.nextSlot = 0;
}

static void endStream(uint8_t port)
{
    streams[port].active = false;
    if (readerPort == (int8_t)port) {
        closeReader();
    }
}

static void startStream(const SdRecord &request)
{
    uint8_t port = request.log.port;
    endStream(port);  // a new request replaces the download on that port

    LogStream &stream = streams[port];
    stream.active = true;
    stream.id = request.log.start;
    stream.pos = request.log.ofs;
    stream.sentAny = false;
    stream.lastCount = 0;

    int slot = (int)stream.id - 1;
    uint32_t size = (slot >= 0 && slot < sdData.fileCount()) ? slotSize(slot) : 0;
    stream.absent = (size == 0);
    stream.slot = (int8_t)slot;

    // The end is fixed here, when the request arrives: what is logged afterwards comes
    // with a later request. count is often 0xFFFFFFFF, so the sum is not formed.
    uint32_t available = (stream.pos < size) ? size - stream.pos : 0;
    if (request.log.count >= available) {
        stream.end = size;
        stream.toFileEnd = true;
    } else {
        stream.end = stream.pos + request.log.count;
        stream.toFileEnd = false;
    }
}

static void postLogData(uint8_t port, uint16_t id, uint32_t ofs, const uint8_t *data, uint8_t count)
{
    LinkMsg intent;
    intent.kind = LinkMsgKind::LogData;
    intent.log_data.id = id;
    intent.log_data.ofs = ofs;
    intent.log_data.count = count;
    memset(intent.log_data.data, 0, sizeof(intent.log_data.data));
    if (count > 0) {
        memcpy(intent.log_data.data, data, count);
    }
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);
}

// One LOG_ENTRY. Returns false when the listing is finished.
static bool serviceListing(uint8_t port)
{
    LogListing &listing = listings[port];

    uint16_t numLogs = 0;
    uint16_t lastLogNum = 0;
    for (int slot = 0; slot < sdData.fileCount(); ++slot) {
        if (SD.exists(slotFileName(slot).c_str())) {
            numLogs++;
            lastLogNum = (uint16_t)(slot + 1);
        }
    }

    while (listing.nextSlot < sdData.fileCount()) {
        int slot = listing.nextSlot++;
        uint16_t id = (uint16_t)(slot + 1);
        if (id < listing.start || id > listing.end) continue;
        uint32_t size = slotSize(slot);
        if (size == 0) continue;

        LinkMsg intent;
        intent.kind = LinkMsgKind::LogEntry;
        intent.log_entry.id = id;
        intent.log_entry.num_logs = numLogs;
        intent.log_entry.last_log_num = lastLogNum;
        intent.log_entry.time_utc = slotTimeUtc(slot);
        intent.log_entry.size = size;
        xQueueSend(linkPorts[port].writeQueue, &intent, 0);
        return true;
    }
    listing.active = false;
    return false;
}

// One LOG_DATA, or nothing when another port's download holds the reading handle.
static void serviceStream(uint8_t port)
{
    LogStream &stream = streams[port];

    if (stream.absent) {
        postLogData(port, stream.id, stream.pos, nullptr, 0);
        stream.active = false;
        return;
    }

    if (stream.pos >= stream.end) {
        // A file whose end falls on a multiple of 90 has not signalled its end yet: an
        // empty chunk does. A range that stops short of the file end is not the end
        // of the log, and must not say so -- MAVProxy closes a download on count 0.
        if (stream.toFileEnd && (!stream.sentAny || stream.lastCount == kLogDataChunk)) {
            postLogData(port, stream.id, stream.pos, nullptr, 0);
        }
        endStream(port);
        return;
    }

    if (readerPort != (int8_t)port) {
        if (readerPort >= 0) return;  // one handle at a time; the other port waits
        reader = SD.open(slotFileName(stream.slot).c_str(), FILE_READ);
        if (!reader) {
            endStream(port);
            return;
        }
        readerPort = (int8_t)port;
        reader.seek(stream.pos);
    }

    uint32_t want = stream.end - stream.pos;
    if (want > kLogDataChunk) want = kLogDataChunk;

    LinkMsg intent;
    intent.kind = LinkMsgKind::LogData;
    intent.log_data.id = stream.id;
    intent.log_data.ofs = stream.pos;
    memset(intent.log_data.data, 0, sizeof(intent.log_data.data));
    int n = reader.read(intent.log_data.data, want);
    if (n <= 0) {
        endStream(port);
        return;
    }
    intent.log_data.count = (uint8_t)n;
    xQueueSend(linkPorts[port].writeQueue, &intent, 0);

    stream.pos += (uint32_t)n;
    stream.sentAny = true;
    stream.lastCount = (uint8_t)n;
}

static bool downloadWorkPending()
{
    for (uint8_t p = 0; p < 2; ++p) {
        if (listings[p].active || streams[p].active) return true;
    }
    return false;
}

static void writeRecord(const SdRecord &incoming)
{
    int before = sdData.currentFile();

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
    default:
        return;
    }

    // A write that rotated has just deleted the slot it moved into. A download of
    // that slot ends here, before another chunk is read: those bytes would belong
    // to the new file, not the one the id named when the download began.
    int after = sdData.currentFile();
    if (after != before) {
        for (uint8_t p = 0; p < 2; ++p) {
            if (streams[p].active && !streams[p].absent && streams[p].slot == after) {
                postLogData(p, streams[p].id, streams[p].pos, nullptr, 0);
                endStream(p);
            }
        }
    }
}

[[noreturn]] void TaskSdWrite(void *pvParameters)
{
    (void) pvParameters;

    // Registered before begin(), so the first file's open is caught too.
    sdData.setOnOpen(writeLogPreamble);
    sdData.begin();

    uint8_t nextPort = 0;

    for (;;)
    {
        // Idle: block until something arrives. Download work pending: look without
        // waiting, or yield one tick when every port with work is paced.
        TickType_t wait = downloadWorkPending() ? 1 : portMAX_DELAY;

        SdRecord incoming;
        while (xQueueReceive(sdWriteQueue, &incoming, wait) == pdPASS) {
            wait = 0;
            switch (incoming.kind) {
            case SdRecordKind::LogList:
                startListing(incoming);
                break;
            case SdRecordKind::LogRead:
                startStream(incoming);
                break;
            case SdRecordKind::LogEnd:
                endStream(incoming.log.port);
                break;
            default:
                writeRecord(incoming);
                break;
            }
        }

        // At most one piece of download work per pass, alternating ports.
        for (uint8_t i = 0; i < 2; ++i) {
            uint8_t port = (uint8_t)((nextPort + i) % 2);
            if (!(listings[port].active || streams[port].active) || !portHasRoom(port)) continue;
            if (listings[port].active) {
                serviceListing(port);
            } else {
                serviceStream(port);
            }
            nextPort = (uint8_t)(port + 1) % 2;
            break;
        }
    }
}
