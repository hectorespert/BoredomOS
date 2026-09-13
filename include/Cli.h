
#ifndef BOREDOMOS_CLI_H
#define BOREDOMOS_CLI_H

// The port carrying the console CLI. Override from build_flags:
// -D CLI_SERIAL=Serial1 moves the CLI to the hardware UART for the bench build,
// where the MAVLink link has taken over Serial (see include/Link.h) -- no build
// may have LINK_SERIAL and CLI_SERIAL name the same port.
// CLI_SERIAL must name a concrete port, never a HardwareSerial reference: a
// base-class reference selects Print::write and transmits one byte at a time.

#ifndef CLI_SERIAL
#define CLI_SERIAL Serial
#endif

#endif //BOREDOMOS_CLI_H
