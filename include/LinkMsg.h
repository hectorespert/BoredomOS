
#ifndef BOREDOMOS_LINKMSG_H
#define BOREDOMOS_LINKMSG_H

#include <stdint.h>

// The outbound queue's item type: what a producer means, not what the wire
// needs. mavlinkPack() (declared in include/MavlinkPack.h, defined in
// src/mavlink.cpp) is the only place that turns one of these into a
// mavlink_message_t -- see design.md, "The outbound queue item becomes a
// tagged union", queue-mavlink-messages-by-value.
enum class LinkMsgKind : uint8_t {
    Heartbeat,
    SystemTime,
    BatteryStatus,
    TimesyncReply,
    StatusText,
    CommandAck,
    NamedValueInt,
};

struct LinkMsg {
    LinkMsgKind kind;
    union {
        // Heartbeat carries no payload: `kind` alone is the whole message.
        // mavlinkPack() re-reads the current recovery/configuration state
        // itself rather than carrying a snapshot of it.
        struct { uint64_t unix_usec; uint32_t boot_ms; } system_time;
        struct { uint16_t millivolts; int8_t remaining; } battery;
        struct { int64_t ts1; uint8_t target_system; uint8_t target_component; } timesync;
        struct { uint8_t severity; char text[50]; } statustext;
        struct { uint16_t command; uint8_t result; } command_ack;
        // name points at one of src/mavlink.cpp's static const char[] name
        // tables -- program-lifetime storage, so a pointer is safe to carry
        // through the queue rather than copying up to 10 bytes.
        struct { const char *name; int32_t value; } named_value_int;
    };
};

// STATUSTEXT (severity + a 50-byte text field) sets this size. A future
// variant that grows past it must grow this assertion deliberately, not
// silently invalidate the RAM math this change and the USB-link backlog
// entry (TODO.md) both depend on.
static_assert(sizeof(LinkMsg) <= 64, "LinkMsg grew past the size the RAM budget assumes");

#endif //BOREDOMOS_LINKMSG_H
