
#ifndef BOREDOMOS_MAVLINK_SHARED_H
#define BOREDOMOS_MAVLINK_SHARED_H

#include <MAVLink.h>

// The message logic both MAVLink endpoints share: what goes out, and what an
// inbound message means. src/mavlink.cpp defines these; src/serial.cpp's radio
// link and src/cli.cpp's USB endpoint are the two callers. Neither transport
// duplicates the identity triple or the inbound switch -- see design.md, "The
// message logic is shared with src/mavlink.cpp, not duplicated".
//
// Named MavlinkShared, not Mavlink: on a case-insensitive filesystem,
// include/Mavlink.h and the okalachev/MAVLink library's own MAVLink.h resolve
// to the same path, and the project's include/ directory wins the lookup --
// silently replacing the real protocol header with this one's few
// declarations. Found by building, per this project's own convention of not
// trusting a name until pio run confirms it.
//
// Each builder fills the mavlink_message_t the caller provides, rather than
// allocating one itself: the radio link passes the heap block it already
// pvPortMalloc()s for serialWriteQueue, and the USB endpoint passes a .bss
// static it packs into a stack buffer and writes directly. Neither transport's
// choice leaks into the other.
//
// Every builder also takes the caller's mavlink_channel_t and packs with the
// library's *_pack_chan() variant, not the plain *_pack() design.md originally
// sketched: plain *_pack() calls mavlink_finalize_message(), which hardcodes
// MAVLINK_COMM_0 as the channel (mavlink_helpers.h) -- every outbound message
// would have shared one sequence counter regardless of which port sent it,
// which is exactly what task 4.4 requires NOT to happen. Found while
// implementing section 4, not anticipated in the original two-argument
// sketch. Serial1 passes MAVLINK_COMM_0 (matching src/serial.cpp's inbound
// parser channel); the USB endpoint passes MAVLINK_COMM_1.

void mavlinkBuildHeartbeat(mavlink_message_t *out, uint8_t chan);
void mavlinkBuildSystemTime(mavlink_message_t *out, uint8_t chan);
void mavlinkBuildBatteryStatus(mavlink_message_t *out, uint8_t chan);
void mavlinkBuildStatusText(mavlink_message_t *out, uint8_t chan, uint8_t severity, const char *text);

// Acts on msg -- including any side effect, such as setting the clock or
// resetting the board -- and returns true when a reply must be sent, having
// filled *reply on replyChan. Both ports call this for every inbound message;
// neither keeps its own copy of what a message means. See design.md's risk
// entry "A ground station on USB during flight is a path that did not exist
// before": this is the whole inbound switch, MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN
// included, not a restricted subset.
//
// *rebootRequested is set true, never acted on here, when msg commands a
// reboot: only the caller knows its own transport's timing -- both need a
// moment for the hardware to actually put the bytes on the wire, not merely
// to leave a software queue, before it is safe to reset. The caller must send
// the reply first (if hasReply), then check *rebootRequested and reset on its
// own schedule.
//
// msg and reply MAY be the same object -- see the definition in
// src/mavlink.cpp for the invariant that makes this safe and what a new case
// must preserve to keep it that way.
bool mavlinkHandleInbound(const mavlink_message_t *msg, uint8_t replyChan, mavlink_message_t *reply, bool *rebootRequested);

#endif //BOREDOMOS_MAVLINK_SHARED_H
