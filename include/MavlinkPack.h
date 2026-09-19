
#ifndef BOREDOMOS_MAVLINKPACK_H
#define BOREDOMOS_MAVLINKPACK_H

#include <MAVLink.h>
#include <LinkMsg.h>

// The only declared entry point into src/mavlink.cpp's protocol knowledge
// reachable from outside that file: turns a queued intent into a wire-ready
// message. src/serial.cpp calls this and nothing else that names a msgid or
// a mavlink_msg_*_pack function -- see ARCHITECTURE.md section 5.1,
// "src/mavlink.cpp owns the protocol. Nothing else in the firmware knows
// what a msgid is."
//
// Named MavlinkPack.h, not Mavlink.h: this filesystem resolves #include
// <MAVLink.h> (the library header) case-insensitively, so a same-named
// header here would shadow it instead of the library's own file being found.
void mavlinkPack(const LinkMsg &intent, mavlink_message_t *out);

#endif //BOREDOMOS_MAVLINKPACK_H
