"""Both ports are independent MAVLink endpoints.

One case per claim replace-console-cli-with-usb-mavlink-link makes that nothing
else checks. Every case here needs *both* ports reachable at once: the board's
own USB CDC port, and a USB-TTL adapter (or the radio) on D0/D1. When the second
one is not attached they raise NoLinkError and are reported as skipped, which is
the honest outcome -- a green run with no adapter has not exercised any of this.

Point HIL_PORT at the adapter to make it the primary link; these cases open the
other port themselves.
"""

import os
import time

from hil import ARDUINO_VID, DEFAULT_BAUD, NoLinkError


def _usb_cdc_port():
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == ARDUINO_VID:
            return p.device
    return None


def _uart_port():
    """A serial adapter that is not the board's own CDC port, or None."""
    from serial.tools import list_ports

    override = os.environ.get("HIL_UART_PORT")
    if override:
        return override

    for p in list_ports.comports():
        if p.vid is None or p.vid == ARDUINO_VID:
            continue
        if "USB" in p.device or "ACM" in p.device or "usbserial" in p.device:
            return p.device
    return None


def _both_ports():
    usb, uart = _usb_cdc_port(), _uart_port()
    if usb is None:
        raise NoLinkError("the board's USB CDC port is not attached")
    if uart is None:
        raise NoLinkError(
            "no USB-TTL adapter found on D0/D1; set HIL_UART_PORT to drive it. "
            "These cases need both ports at once and cannot run on one."
        )
    return usb, uart


def _connect(port, baud):
    from pymavlink import mavutil

    return mavutil.mavlink_connection(port, baud=baud)


def _collect(conn, seconds, want=None):
    """Return {type: [messages]} seen within `seconds`."""
    out = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        msg = conn.recv_match(blocking=True, timeout=0.5)
        if msg is None or msg.get_type() == "BAD_DATA":
            continue
        out.setdefault(msg.get_type(), []).append(msg)
        if want and all(k in out for k in want):
            break
    return out


def test_both_ports_emit_the_same_cadences(link):
    """Each port carries the full telemetry set, independently of the other."""
    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        seen_a = _collect(a, 12.0)
        seen_b = _collect(b, 12.0)
    finally:
        a.close()
        b.close()

    for name, seen in (("USB", seen_a), ("UART", seen_b)):
        assert "HEARTBEAT" in seen, f"no HEARTBEAT on {name} in 12 s"
        assert "SYSTEM_TIME" in seen, f"no SYSTEM_TIME on {name} in 12 s"
        assert len(seen["HEARTBEAT"]) >= 8, (
            f"{name} saw {len(seen['HEARTBEAT'])} HEARTBEATs in 12 s, expected ~12 "
            "(1 Hz)"
        )


def test_each_port_has_its_own_sequence(link):
    """Sequence numbers advance per port, not from one shared counter.

    This is what a missed mavlink_msg_*_pack_chan conversion breaks, and it
    breaks silently: both ground stations still receive every frame, they just
    each see gaps and infer packet loss.
    """
    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        seq_a = [m.get_seq() for m in _collect(a, 10.0).get("HEARTBEAT", [])]
        seq_b = [m.get_seq() for m in _collect(b, 10.0).get("HEARTBEAT", [])]
    finally:
        a.close()
        b.close()

    for name, seq in (("USB", seq_a), ("UART", seq_b)):
        assert len(seq) >= 4, f"{name}: only {len(seq)} HEARTBEATs, need 4 to compare"
        # HEARTBEAT is one of four entries on each port's schedule, so its own
        # seq advances by more than 1 between sightings -- but the step must be
        # small and constant. A shared counter makes it roughly double.
        steps = [(y - x) % 256 for x, y in zip(seq, seq[1:])]
        assert max(steps) <= 6, (
            f"{name} HEARTBEAT sequence steps {steps}: too large for a per-port "
            "counter, which is what a shared current_tx_seq looks like"
        )


def test_reply_goes_out_the_port_it_arrived_on(link):
    """A TIMESYNC answered on one port is not echoed on the other."""
    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        _collect(a, 1.0)
        _collect(b, 1.0)

        # Ask on USB only.
        a.mav.timesync_send(0, int(time.time() * 1e9), 1, 1)

        got_a = _collect(a, 5.0, want=["TIMESYNC"])
        got_b = _collect(b, 5.0, want=["TIMESYNC"])
    finally:
        a.close()
        b.close()

    assert "TIMESYNC" in got_a, "the port that asked did not get a TIMESYNC reply"
    assert "TIMESYNC" not in got_b, (
        "the UART received a TIMESYNC reply to a request that arrived on USB; "
        "replies must leave by the port they came in on"
    )


def test_housekeeping_arming_is_per_port(link):
    """Arming NAMED_VALUE_INT on one port does not arm the other."""
    from pymavlink import mavutil

    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        _collect(a, 1.0)
        _collect(b, 1.0)

        a.mav.command_long_send(
            1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
            mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL, 0,
            mavutil.mavlink.MAVLINK_MSG_ID_NAMED_VALUE_INT,
            1_000_000, 0, 0, 0, 0, 0,
        )

        got_a = _collect(a, 8.0)
        got_b = _collect(b, 8.0)
    finally:
        # Leave the board as it was found.
        try:
            a2 = _connect(usb, 115200)
            a2.mav.command_long_send(
                1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
                mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL, 0,
                mavutil.mavlink.MAVLINK_MSG_ID_NAMED_VALUE_INT,
                -1, 0, 0, 0, 0, 0,
            )
            a2.close()
        except Exception:
            pass
        a.close()
        b.close()

    assert "NAMED_VALUE_INT" in got_a, "the armed port sent no NAMED_VALUE_INT"
    assert "NAMED_VALUE_INT" not in got_b, (
        "the unarmed port sent NAMED_VALUE_INT; arming must be per port"
    )
