
#ifndef BOREDOMOS_LINK_H
#define BOREDOMOS_LINK_H

// The port carrying the MAVLink link and its speed. Override from build_flags:
// -D LINK_SERIAL=Serial puts the link back on USB CDC for bench work.
// LINK_SERIAL must name a concrete port, never a HardwareSerial reference: a
// base-class reference selects Print::write and transmits one byte at a time.

#ifndef LINK_SERIAL
#define LINK_SERIAL Serial1
#endif

#ifndef LINK_BAUD
#define LINK_BAUD 57600
#endif

#endif //BOREDOMOS_LINK_H
