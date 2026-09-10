"""The USB CDC port carries nothing.

Covers the mavlink-link scenario "Host attached to USB ... receives no MAVLink
frames ... and no text". This is the check that shows the link really left USB:
the telemetry checks prove frames arrive somewhere, this one proves they do not
arrive here.

Skipped when the link itself is on USB, which is what
-D LINK_SERIAL=Serial produces: the port is then supposed to carry frames.
"""

import time

from hil import ARDUINO_VID, NoLinkError


def _usb_cdc_port():
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == ARDUINO_VID:
            return p.device
    return None


def test_usb_console_is_silent(link):
    if link.is_usb_cdc:
        raise NoLinkError("the link is on USB in this build, so it must not be silent")

    port = _usb_cdc_port()
    if port is None:
        raise NoLinkError("the board's USB CDC port is not attached")

    import serial

    with serial.Serial(port, 115200, timeout=1) as console:
        console.dtr = True
        time.sleep(0.5)
        console.reset_input_buffer()
        data = b""
        started = time.time()
        while time.time() - started < 8.0:
            data += console.read(256)

    assert not data, (
        f"{len(data)} bytes on the USB console, which nothing should write to: "
        f"{data[:64]!r}"
    )
