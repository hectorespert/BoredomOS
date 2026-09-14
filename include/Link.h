
#ifndef BOREDOMOS_LINK_H
#define BOREDOMOS_LINK_H

// The radio link is fixed to Serial1: add-usb-dual-protocol gave the USB port
// its own permanent MAVLink endpoint (see include/Cli.h), so there is no
// longer a build that needs the radio link moved onto USB, and no override
// point for the port. LINK_BAUD stays a build-time override.

#define LINK_SERIAL Serial1

#ifndef LINK_BAUD
#define LINK_BAUD 57600
#endif

#endif //BOREDOMOS_LINK_H
