
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
    // Carries no payload, like Heartbeat: every AUTOPILOT_VERSION field this
    // firmware can honestly fill is a compile-time constant, so mavlinkPack()
    // builds the whole message itself rather than reading one from `intent`.
    AutopilotVersion,
    SysStatus,
    // Sent only in reply to MAV_CMD_GET_MESSAGE_INTERVAL.
    MessageInterval,
    // Sent only in reply to MISSION_REQUEST_LIST. This firmware holds no
    // mission, so the count is always 0 and mavlinkPack() writes it itself.
    MissionCount,
    // Carries no payload, like AutopilotVersion: every PROTOCOL_VERSION field
    // is a constant.
    ProtocolVersion,
    // Both posted by TaskSdWrite, the card's owner, in answer to the log
    // protocol (download-the-flight-log). LogData carries its 90 bytes by value.
    LogEntry,
    LogData,
};

struct LinkMsg {
    LinkMsgKind kind;
    union {
        // Heartbeat carries no payload: `kind` alone is the whole message.
        // mavlinkPack() re-reads the current recovery/configuration state
        // itself rather than carrying a snapshot of it.
        struct { uint64_t unix_usec; uint32_t boot_ms; } system_time;
        struct { uint16_t millivolts; int8_t remaining; } battery;
        // tc1 is captured by TaskMavlink when the request is received, not read
        // by mavlinkPack() when the reply is formed: the ground computes its
        // offset from tc1 against ts1 plus half the round trip, so reading the
        // clock after a queue hop would put this board's scheduling latency
        // inside the number as if it were clock offset. Both reference
        // autopilots timestamp on receipt for the same reason.
        struct { int64_t tc1; int64_t ts1; uint8_t target_system; uint8_t target_component; } timesync;
        struct { uint8_t severity; char text[50]; } statustext;
        struct { uint16_t command; uint8_t result; } command_ack;
        // name points at one of src/mavlink.cpp's static const char[] name
        // tables -- program-lifetime storage, so a pointer is safe to carry
        // through the queue rather than copying up to 10 bytes.
        struct { const char *name; int32_t value; } named_value_int;
        // sensors carries the one bitmap value this firmware reports for
        // present/enabled/health alike -- see design.md, emit-sys-status.
        // load, drop_rate_comm, errors_count2..4 and the *_extended bitmaps
        // are always 0 and are not carried through the queue; mavlinkPack()
        // fills them directly.
        struct { uint32_t sensors; uint16_t voltage_mv; int8_t battery_remaining; uint16_t errors_comm; uint16_t errors_count1; } sys_status;
        struct { int32_t interval_us; uint16_t message_id; } message_interval;
        struct { uint8_t target_system; uint8_t target_component; uint8_t mission_type; } mission_count;
        struct { uint32_t time_utc; uint32_t size; uint16_t id; uint16_t num_logs; uint16_t last_log_num; } log_entry;
        struct { uint32_t ofs; uint16_t id; uint8_t count; uint8_t data[90]; } log_data;
    };
};

// LOG_DATA (90 bytes of log plus offset, id and count) sets this size since
// download-the-flight-log; STATUSTEXT set it before. Every write queue's storage
// is depth x this, and every task that holds a LinkMsg local pays it on its
// stack, so it must grow deliberately, never silently.
static_assert(sizeof(LinkMsg) <= 112, "LinkMsg grew past the size the RAM budget assumes");

#endif //BOREDOMOS_LINKMSG_H
