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
        reduced = any(m.system_status == 6 for m in seen_a.get("HEARTBEAT", []))
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
        # The full set, not just the 1 Hz pair: a firmware that emitted
        # BATTERY_STATUS on one port only would otherwise pass this case, which
        # is exactly the asymmetry it exists to reject. Skipped when the board
        # is in the reduced configuration, where the schedule withholds it by
        # design (specs/mavlink-link/spec.md).
        if not reduced:
            assert "BATTERY_STATUS" in seen, f"no BATTERY_STATUS on {name} in 12 s"
            assert len(seen["BATTERY_STATUS"]) >= 4, (
                f"{name} saw {len(seen['BATTERY_STATUS'])} BATTERY_STATUS in 12 s, "
                "expected ~6 (0.5 Hz)"
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
        # Each port's own schedule emits 2.5 frames a second -- HEARTBEAT and
        # SYSTEM_TIME at 1 Hz, BATTERY_STATUS at 0.5 Hz -- so between two
        # consecutive HEARTBEATs on one port exactly 2 or 3 frames leave it.
        # With a shared current_tx_seq both ports' traffic advances the same
        # counter and the step becomes 5 or 6.
        #
        # The bound has to sit below that or the case cannot fail for the
        # reason it exists: an earlier version used <= 6, which the shared
        # counter passes. 4 leaves one frame of slack (an unsolicited
        # STATUSTEXT, say) while still rejecting 5.
        steps = [(y - x) % 256 for x, y in zip(seq, seq[1:])]
        assert max(steps) <= 4, (
            f"{name} HEARTBEAT sequence steps {steps}: a per-port counter steps "
            "2-3, a counter shared with the other port steps 5-6. This is what a "
            "missed mavlink_msg_*_pack_chan conversion looks like."
        )


def test_reply_goes_out_the_port_it_arrived_on(link):
    """A TIMESYNC answered on one port is not echoed on the other."""
    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        _collect(a, 1.0)
        _collect(b, 1.0)

        # Ask on USB only. timesync_send takes (tc1, ts1) in this dialect --
        # the same two-argument form check_timesync.py uses. Passing the
        # target system and component positionally raises TypeError before any
        # assertion runs.
        a.mav.timesync_send(0, int(time.time() * 1e9))

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


def test_autopilot_version_goes_out_the_port_it_arrived_on(link):
    """An AUTOPILOT_VERSION reply to MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES is
    not echoed on the other port -- the same claim
    test_reply_goes_out_the_port_it_arrived_on makes for TIMESYNC, for the
    mavlink-link requirement answer-autopilot-version-requests adds.
    """
    from pymavlink import mavutil

    usb, uart = _both_ports()
    a, b = _connect(usb, 115200), _connect(uart, DEFAULT_BAUD)
    try:
        _collect(a, 1.0)
        _collect(b, 1.0)

        a.mav.command_long_send(
            1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
            mavutil.mavlink.MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES, 0,
            1, 0, 0, 0, 0, 0, 0,
        )

        got_a = _collect(a, 5.0, want=["AUTOPILOT_VERSION"])
        got_b = _collect(b, 5.0, want=["AUTOPILOT_VERSION"])
    finally:
        a.close()
        b.close()

    assert "AUTOPILOT_VERSION" in got_a, "the port that asked did not get AUTOPILOT_VERSION"
    assert "AUTOPILOT_VERSION" not in got_b, (
        "the UART received AUTOPILOT_VERSION for a request that arrived on USB; "
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
