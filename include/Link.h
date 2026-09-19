
#ifndef BOREDOMOS_LINK_H
#define BOREDOMOS_LINK_H

// The two ports carrying MAVLink, and the UART's speed. Both are named once
// here and nowhere else, so a build can move the UART link without touching
// protocol code or a task body -- see specs/mavlink-link/spec.md, "Each
// MAVLink port is named by a single definition".
//
// LINK_UART and LINK_USB must each stay a macro naming a concrete port, never
// a HardwareSerial reference: UART overloads write(uint8_t*, size_t) as
// non-const and never overrides Print's virtual const version, so reaching a
// port through a base-class reference silently selects the per-byte fallback.
// Serial and Serial1 are different types besides. src/link.cpp wraps each in a
// function that names it concretely, and include/LinkPort.h carries the
// pointers -- that indirection is what keeps the block write.
//
// LINK_BAUD applies to LINK_UART alone. A USB CDC port has no line rate, so
// nothing sets one on LINK_USB.

#ifndef LINK_UART
#define LINK_UART Serial1
#endif

#ifndef LINK_BAUD
#define LINK_BAUD 57600
#endif

#ifndef LINK_USB
#define LINK_USB Serial
#endif

// Channel numbers into the MAVLink library's per-channel parser and sequence
// state. These index m_mavlink_buffer/m_mavlink_status, so
// MAVLINK_COMM_NUM_BUFFERS in platformio.ini must be at least as large as the
// count here -- it is 2, and raising the count means raising that too.
#define LINK_CHAN_UART MAVLINK_COMM_0
#define LINK_CHAN_USB  MAVLINK_COMM_1

#endif //BOREDOMOS_LINK_H
