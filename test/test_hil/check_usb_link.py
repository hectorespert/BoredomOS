"""The USB CDC port carries MAVLink.

Covers the mavlink-link scenario "Ground station attached to USB": a host that
opens the port and speaks MAVLink receives the telemetry set and no text.

This file replaces check_silence.py, which asserted the exact opposite -- that
USB carried no frames -- and was correct until
replace-console-cli-with-usb-mavlink-link made USB a second MAVLink endpoint.
It is inverted rather than deleted because the claim it guards is the one this
change most needs held: that the flight build is reachable over the USB cable
with no adapter attached.
"""

import time

from hil import ARDUINO_VID, NoLinkError


def _usb_cdc_port():
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == ARDUINO_VID:
            return p.device
    return None


def test_usb_carries_mavlink(link):
    """A fresh connection to the board's own CDC port sees MAVLink frames."""
    port = _usb_cdc_port()
    if port is None:
        raise NoLinkError("the board's USB CDC port is not attached")

    from pymavlink import mavutil

    conn = mavutil.mavlink_connection(port, baud=115200)
    try:
        deadline = time.time() + 8.0
        seen = set()
        while time.time() < deadline and "HEARTBEAT" not in seen:
            msg = conn.recv_match(blocking=True, timeout=1.0)
            if msg is not None and msg.get_type() != "BAD_DATA":
                seen.add(msg.get_type())
    finally:
        conn.close()

    assert "HEARTBEAT" in seen, (
        f"no HEARTBEAT on the USB CDC port within 8 s; saw {sorted(seen) or 'nothing'}. "
        "USB is supposed to carry MAVLink in every build since "
        "replace-console-cli-with-usb-mavlink-link."
    )


def test_usb_carries_no_text(link):
    """Nothing on USB is a text console reply: there is no CLI any more."""
    port = _usb_cdc_port()
    if port is None:
        raise NoLinkError("the board's USB CDC port is not attached")

    import serial

    with serial.Serial(port, 115200, timeout=1) as console:
        console.dtr = True
        time.sleep(0.5)
        console.reset_input_buffer()
        console.write(b"ps\n")
        time.sleep(1.0)
        data = console.read(512)

    # MAVLink 2 frames start with 0xFD, so bytes are expected -- what must not
    # appear is the CLI's own reply to `ps`, which would mean a console is still
    # answering on a port this change made a link.
    assert b"unused" not in data.lower() and b"heap" not in data.lower(), (
        f"the USB port answered `ps` with what looks like CLI text: {data[:96]!r}"
    )
