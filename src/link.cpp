#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <MAVLink.h>
#include <Serial.h>
#include <Link.h>
#include <LinkMsg.h>
#include <LinkPort.h>
#include <MavlinkPack.h>
#include <limits.h>

// This file owns both link ports. Not one file per port: two owners of the
// same class of resource is what would force mutexes, and the absence of them
// here rests on single ownership (design.md, Decision 11).

extern QueueHandle_t linkReadQueue;

// Per-port wrappers. Each names its concrete port, which is the whole point --
// see include/Link.h and include/LinkPort.h for what a HardwareSerial& would
// silently cost.
static int    uartAvailable()                              { return LINK_UART.available(); }
static int    uartRead()                                   { return LINK_UART.read(); }
static size_t uartWrite(const uint8_t *b, size_t n)        { return LINK_UART.write(b, n); }

// A UART's write busy-waits for as long as the baud rate takes and then
// returns, so there is nothing to guard against: it always accepts the frame.
// INT_MAX makes the shared writer body's check a no-op here without an
// if (chan == ...) inside it (design.md, Decision 6).
static int    uartAvailableForWrite()                      { return INT_MAX; }

static int    usbAvailable()                               { return LINK_USB.available(); }
static int    usbRead()                                    { return LINK_USB.read(); }
static size_t usbWrite(const uint8_t *b, size_t n)         { return LINK_USB.write(b, n); }
static int    usbAvailableForWrite()                       { return LINK_USB.availableForWrite(); }

// Filled by linkPortsInit() before the scheduler starts, then read-only.
LinkPort linkPorts[2];

void linkPortsInit(QueueHandle_t uartWriteQueue, QueueHandle_t usbWriteQueue)
{
    linkPorts[0].available         = uartAvailable;
    linkPorts[0].read              = uartRead;
    linkPorts[0].write             = uartWrite;
    linkPorts[0].availableForWrite = uartAvailableForWrite;
    linkPorts[0].writeQueue        = uartWriteQueue;
    linkPorts[0].rx.chan           = LINK_CHAN_UART;

    linkPorts[1].available         = usbAvailable;
    linkPorts[1].read              = usbRead;
    linkPorts[1].write             = usbWrite;
    linkPorts[1].availableForWrite = usbAvailableForWrite;
    linkPorts[1].writeQueue        = usbWriteQueue;
    linkPorts[1].rx.chan           = LINK_CHAN_USB;
}

// How many bytes one pass drains from one port. Without a cap the loop is
// unbounded: LINK_UART at 57600 can only deliver 58 B into a 10 ms poll so it
// never bites there, but USB CDC bursts far past that and this reader runs
// above TaskLogger and TaskSdWrite. 128 B per pass is 12.8 kB/s sustained,
// against an inbound stream of a few frames a second (design.md, Decision 8).
static constexpr int kMaxBytesPerPass = 128;

[[noreturn]] void TaskLinkRead(void *pvParameters)
{
    LinkPort &port = *static_cast<LinkPort *>(pvParameters);

    for (;;)
    {
        int drained = 0;
        while (port.available() > 0 && drained < kMaxBytesPerPass)
        {
            uint8_t receivedByte = (uint8_t)port.read();
            ++drained;

            // Parsed straight into port.rx.msg and posted from there. Never a
            // local: 291 B on a 384 B stack overflows it (design.md,
            // Decision 7). Two readers share this body but not this storage --
            // each has its own LinkPort, which is what makes the file-static
            // buffer this replaced unsafe now that two tasks parse.
            if (mavlink_parse_char(port.rx.chan, receivedByte, &port.rx.msg, &port.rxstatus)) {
                xQueueSend(linkReadQueue, &port.rx, 0);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

[[noreturn]] void TaskLinkWrite(void *pvParameters)
{
    LinkPort &port = *static_cast<LinkPort *>(pvParameters);

    for (;;)
    {
        LinkMsg intent;
        if (xQueueReceive(port.writeQueue, &intent, portMAX_DELAY))
        {
            mavlink_message_t msg_to_send;
            mavlinkPack(port.rx.chan, intent, &msg_to_send);

            uint8_t buf[MAVLINK_MAX_PACKET_LEN];
            uint16_t len = mavlink_msg_to_send_buffer(buf, &msg_to_send);

            // Ask before writing. _SerialUSB::write() loops without yielding
            // while its buffer is full, so a host that opens the port and
            // stops reading would starve the idle hook and let the watchdog
            // reset the board. Dropping the frame is what already happens when
            // the outbound queue fills, so this is the existing failure mode
            // rather than a new one (design.md, Decision 6).
            if (port.availableForWrite() < (int)len) {
                continue;
            }

            port.write(buf, len);
        }
    }
}
