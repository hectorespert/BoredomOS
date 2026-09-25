
#ifndef BOREDOMOS_LINKPORT_H
#define BOREDOMOS_LINKPORT_H

#include <stdint.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>

// What the inbound queue carries: a parsed message plus the channel it arrived
// on, so TaskMavlink's dispatch can answer on the port that asked. One shared
// queue rather than one per port -- see design.md, Decision 1: a second 8-deep
// mavlink_message_t queue would cost 2328 B to distinguish what this tag
// distinguishes for the price of its alignment padding.
struct InboundMsg {
    uint8_t chan;
    mavlink_message_t msg;
};

// One per port. Everything that differs between Serial1 and the USB CDC port
// lives here, so src/link.cpp has one reader body and one writer body rather
// than two of each.
//
// The port is reached through function pointers, never a HardwareSerial& or a
// Stream&. include/Link.h explains why: UART overloads
// write(uint8_t*, size_t) as non-const and never overrides Print's virtual
// const version, so a base-class reference silently selects the per-byte
// fallback. Serial and Serial1 are different types in any case. The wrappers
// in src/link.cpp name their concrete port, which is what keeps the block
// write (design.md, Decision 3).
struct LinkPort {
    int    (*available)();
    int    (*read)();

    // uint8_t *, deliberately not const uint8_t *. UART declares
    // write(uint8_t *, size_t) as NON-const, which hides rather than overrides
    // Print's virtual const version -- so a const pointer cannot bind to it and
    // resolves to Print::write(const uint8_t *, size_t), the per-byte fallback
    // this whole indirection exists to avoid (include/Link.h). Verified in the
    // disassembly, not assumed: with a const parameter the wrapper tail-called
    // arduino::Print::write; with this one it tail-calls
    // UART::write(unsigned char*, unsigned int). _SerialUSB's override IS
    // const, so it takes a mutable pointer happily either way. The caller's
    // buffer is a plain local array, so nothing is lost by not marking it
    // const.
    size_t (*write)(uint8_t *buffer, size_t size);

    // How many bytes the port will accept right now without blocking. The USB
    // CDC implementation busy-waits without yielding when its buffer is full
    // (cores/arduino/usb/SerialUSB.cpp), which above idle priority starves the
    // idle hook and lets the watchdog reset the board -- so the writer asks
    // first and drops the frame instead. A UART's write is bounded by the baud
    // rate and needs no guard, so its wrapper returns INT_MAX
    // (design.md, Decision 6).
    int    (*availableForWrite)();

    // The queue this port's writer drains. Dispatch posts a reply here after
    // reading the originating chan off the inbound item.
    QueueHandle_t writeQueue;

    // The reader parses into rx.msg in place and posts &rx. It is not a local:
    // a mavlink_message_t is 291 B and the reader runs on a 384 B stack, so
    // composing the item on the stack would overflow it (design.md,
    // Decision 7). rx.chan is set once at init and is the single copy of this
    // port's channel number -- mavlink_parse_char is called with it too.
    InboundMsg       rx;
    mavlink_status_t rxstatus;

    // Accumulated separately from rxstatus.packet_rx_drop_count, which the
    // library resets to 0 after every single byte (mavlink_helpers.h copies
    // status->parse_error out and clears it on each mavlink_parse_char call) --
    // reading it from a different task's independent schedule, as
    // sendSysStatus does, would see zero except in the microsecond window
    // right after an erroring byte. TaskLinkRead adds rxstatus's per-byte
    // value in here after every mavlink_parse_char call, so it holds a real
    // running total. Saturating like writeDropCount, for the same reason: a
    // wrapped counter reads as 0 -- healthy -- right when it has the most to
    // report. Written only by this port's own TaskLinkRead, read only by
    // TaskMavlink's sendSysStatus -- single writer, atomic uint16_t reads, no
    // mutex needed, same reasoning as every other cross-task flag in this
    // firmware.
    uint16_t rxDropCount;
};

#endif //BOREDOMOS_LINKPORT_H
