"""Messages and intervals on request: REQUEST_MESSAGE, GET_MESSAGE_INTERVAL,
PROTOCOL_VERSION and MISSION_REQUEST_LIST.

Covers the mavlink-link spec requirements "`MAV_CMD_REQUEST_MESSAGE` sends the
requested message once", "`MAV_CMD_GET_MESSAGE_INTERVAL` reports the interval a
message really has", "`PROTOCOL_VERSION` is sent on request and reports MAVLink 2
only" and "A mission list request is answered with an empty mission"
(openspec/changes/answer-message-requests/specs/mavlink-link/spec.md).

The per-port scenarios need both ports at once and live in check_dual_link.py;
the reduced-configuration ones need the board put there by hand and self-skip
otherwise, like their neighbours.

**Why the requests come in bursts.** HEARTBEAT, SYSTEM_TIME, SYS_STATUS and
BATTERY_STATUS also leave on their own schedule, so one message of that id after a
request proves nothing: it may be the periodic one. A burst of five requests
inside about a second and a half yields at least five messages of the id only if
each request produced one, since the schedule supplies two at most. AUTOPILOT_VERSION,
PROTOCOL_VERSION and MESSAGE_INTERVAL are never periodic, so for those a single
message is already proof.

**State.** Arming the housekeeping stream survives until the board resets, so the one
case that arms it disarms in a `finally`, and every case here that asserts a state
first reads it back rather than assuming it.

Listens fresh, never through link.sample()'s cached window, the way
check_capabilities.py and check_unsupported_commands.py do.
"""

import struct
import time

from hil import NoLinkError

PROTOCOL_VERSION_ID = 300
BURST = 5
BURST_SPACING = 0.15


def _mavlink():
    from pymavlink import mavutil

    return mavutil.mavlink


def _drain(link):
    """Discard what is already buffered, so an old reply cannot answer a new request."""
    while link.mav.recv_match(blocking=False) is not None:
        pass


def _collect(link, seconds):
    """Every decodable message seen within `seconds`, in arrival order."""
    out = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        msg = link.mav.recv_match(blocking=True, timeout=max(0.05, deadline - time.time()))
        if msg is not None and msg.get_type() != "BAD_DATA":
            out.append(msg)
    return out


def _send(link, command, param1, param2=0.0, param7=0.0):
    mav = _mavlink()
    link.mav.mav.command_long_send(
        1, mav.MAV_COMP_ID_AUTOPILOT1, command, 0,
        param1, param2, 0, 0, 0, 0, param7,
    )


def _exchange(link, command, param1, seconds=1.5, param7=0.0):
    """Send one command and return everything that arrives afterwards."""
    _drain(link)
    _send(link, command, param1, param7=param7)
    return _collect(link, seconds)


def _acks(msgs, command):
    return [m for m in msgs if m.get_type() == "COMMAND_ACK" and m.command == command]


def _of(msgs, name):
    return [m for m in msgs if m.get_type() == name]


def _name_of(msgid):
    """The type name pymavlink gives a message of this id.

    The dialect the harness decodes with (ardupilotmega) does not know every id the
    firmware sends: PROTOCOL_VERSION (300) arrives as UNKNOWN_300, and an id no
    dialect has never arrives at all. Both are named here rather than looked up
    blindly, so a case can still say "none of that id was sent".
    """
    known = _mavlink().mavlink_map.get(msgid)
    return known.msgname if known is not None else f"UNKNOWN_{msgid}"


def _require_normal(link):
    """Cases about streams need the normal configuration; the reduced one has its own."""
    mav = _mavlink()
    hb = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=15.0)
    if hb is None:
        raise NoLinkError("no HEARTBEAT seen")
    if hb.system_status != mav.MAV_STATE_ACTIVE:
        raise NoLinkError("board is not in the normal configuration")


def _require_reduced(link):
    mav = _mavlink()
    hb = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=15.0)
    if hb is None:
        raise NoLinkError("no HEARTBEAT seen")
    if hb.system_status != mav.MAV_STATE_CRITICAL:
        raise NoLinkError(
            "board is not in the reduced configuration -- remove the SD card and reset "
            "it (see check_housekeeping.py's reduced-configuration case), and put it back after"
        )


def _burst(link, msgid, count=BURST):
    """`count` REQUEST_MESSAGE for `msgid`, spaced out; returns everything received."""
    mav = _mavlink()
    _drain(link)
    seen = []
    for _ in range(count):
        _send(link, mav.MAV_CMD_REQUEST_MESSAGE, float(msgid))
        seen += _collect(link, BURST_SPACING)
    seen += _collect(link, 1.0)
    return seen


def _interval_of(link, msgid, seconds=1.5):
    """GET_MESSAGE_INTERVAL for `msgid`; returns (acks, MESSAGE_INTERVAL messages, all)."""
    mav = _mavlink()
    msgs = _exchange(link, mav.MAV_CMD_GET_MESSAGE_INTERVAL, float(msgid), seconds)
    return (
        _acks(msgs, mav.MAV_CMD_GET_MESSAGE_INTERVAL),
        [m for m in _of(msgs, "MESSAGE_INTERVAL") if m.message_id == msgid],
        msgs,
    )


# --- MAV_CMD_REQUEST_MESSAGE ---------------------------------------------------------


def test_every_served_id_is_sent_once_per_request(link):
    _require_normal(link)
    mav = _mavlink()

    problems = []
    for msgid in (0, 1, 2, 147, 148, 300):
        msgs = _burst(link, msgid)
        acks = _acks(msgs, mav.MAV_CMD_REQUEST_MESSAGE)
        got = len(_of(msgs, _name_of(msgid)))
        if len(acks) != BURST:
            problems.append(f"id {msgid}: {len(acks)} COMMAND_ACK for {BURST} requests")
        elif any(a.result != mav.MAV_RESULT_ACCEPTED for a in acks):
            problems.append(f"id {msgid}: results {[a.result for a in acks]}, expected all ACCEPTED")
        if got < BURST:
            problems.append(f"id {msgid}: {got} {_name_of(msgid)} for {BURST} requests")
    assert not problems, "; ".join(problems)


def test_requests_do_not_disturb_the_periodic_cadences(link):
    _require_normal(link)
    mav = _mavlink()

    _drain(link)
    seen = []
    started = time.time()
    for _ in range(60):
        _send(link, mav.MAV_CMD_REQUEST_MESSAGE, 0.0)
        seen += _collect(link, 0.1)
    elapsed = time.time() - started

    # 1 Hz over ~6 s: 5 to 7 allows for the window's edges and nothing else.
    low, high = int(elapsed) - 1, int(elapsed) + 1
    for name in ("SYSTEM_TIME", "SYS_STATUS"):
        n = len(_of(seen, name))
        assert low <= n <= high, (
            f"{n} {name} in {elapsed:.1f} s of REQUEST_MESSAGE traffic, expected {low}-{high}"
        )


def test_unserved_ids_are_denied_and_send_nothing(link):
    mav = _mavlink()
    # 252 is housekeeping: no one-shot form. 24 and 65535 are ids this firmware never sends.
    for msgid in (252, 24, 65535):
        msgs = _exchange(link, mav.MAV_CMD_REQUEST_MESSAGE, float(msgid))
        acks = _acks(msgs, mav.MAV_CMD_REQUEST_MESSAGE)
        assert acks, f"no COMMAND_ACK for a request of id {msgid}"
        assert acks[0].result == mav.MAV_RESULT_DENIED, (
            f"id {msgid}: expected MAV_RESULT_DENIED, got {acks[0].result}"
        )
        assert not _of(msgs, _name_of(msgid)), f"a {_name_of(msgid)} was sent for a DENIED request"


def test_an_id_that_is_not_an_id_is_denied(link):
    """65684 is 65536 + 148: narrowed to 16 bits it would be AUTOPILOT_VERSION, which is
    never periodic, so seeing one is unambiguous."""
    mav = _mavlink()
    for param in (65684.0, float("nan"), -1.0, 2.5):
        msgs = _exchange(link, mav.MAV_CMD_REQUEST_MESSAGE, param)
        acks = _acks(msgs, mav.MAV_CMD_REQUEST_MESSAGE)
        assert acks, f"no COMMAND_ACK for message id parameter {param}"
        assert acks[0].result == mav.MAV_RESULT_DENIED, (
            f"parameter {param}: expected MAV_RESULT_DENIED, got {acks[0].result}"
        )
        assert not _of(msgs, "AUTOPILOT_VERSION"), (
            f"parameter {param} was read as a valid id and answered"
        )


# --- MAV_CMD_GET_MESSAGE_INTERVAL ---------------------------------------------------


def test_get_interval_reports_the_periodic_rates(link):
    _require_normal(link)
    mav = _mavlink()

    for msgid, expected in ((0, 1_000_000), (1, 1_000_000), (2, 1_000_000), (147, 2_000_000)):
        acks, intervals, msgs = _interval_of(link, msgid)
        assert acks, f"no COMMAND_ACK for GET_MESSAGE_INTERVAL of id {msgid}"
        assert acks[0].result == mav.MAV_RESULT_ACCEPTED, f"id {msgid}: result {acks[0].result}"
        assert intervals, f"no MESSAGE_INTERVAL for id {msgid}"
        assert intervals[0].interval_us == expected, (
            f"id {msgid}: interval_us {intervals[0].interval_us}, expected {expected}"
        )
        # The command's definition: ACK first, then the MESSAGE_INTERVAL.
        assert msgs.index(acks[0]) < msgs.index(intervals[0]), (
            f"id {msgid}: MESSAGE_INTERVAL arrived before its COMMAND_ACK"
        )


def test_get_interval_of_on_request_and_unavailable_messages(link):
    mav = _mavlink()
    # 148 and 300 are served on request but never stream: off. 244 is only ever a
    # reply, 24 and 65535 are never sent: not available.
    for msgid, expected in ((148, -1), (300, -1), (244, 0), (24, 0), (65535, 0)):
        acks, intervals, _ = _interval_of(link, msgid)
        assert acks and acks[0].result == mav.MAV_RESULT_ACCEPTED, (
            f"id {msgid}: no ACCEPTED COMMAND_ACK"
        )
        assert intervals, f"no MESSAGE_INTERVAL for id {msgid}"
        assert intervals[0].interval_us == expected, (
            f"id {msgid}: interval_us {intervals[0].interval_us}, expected {expected}"
        )


def test_get_interval_of_an_id_that_is_not_an_id_is_denied(link):
    """65538 is 65536 + 2: read through 16 bits it would be SYSTEM_TIME."""
    mav = _mavlink()
    for param in (65538.0, float("nan"), -1.0):
        msgs = _exchange(link, mav.MAV_CMD_GET_MESSAGE_INTERVAL, param)
        acks = _acks(msgs, mav.MAV_CMD_GET_MESSAGE_INTERVAL)
        assert acks, f"no COMMAND_ACK for message id parameter {param}"
        assert acks[0].result == mav.MAV_RESULT_DENIED, (
            f"parameter {param}: expected MAV_RESULT_DENIED, got {acks[0].result}"
        )
        assert not _of(msgs, "MESSAGE_INTERVAL"), f"parameter {param} was answered with an interval"


def test_get_interval_follows_the_housekeeping_stream(link):
    mav = _mavlink()
    named = float(mav.MAVLINK_MSG_ID_NAMED_VALUE_INT)

    def housekeeping_interval():
        _, intervals, _ = _interval_of(link, 252)
        assert intervals, "no MESSAGE_INTERVAL for NAMED_VALUE_INT"
        return intervals[0].interval_us

    def set_interval(us):
        _drain(link)
        _send(link, mav.MAV_CMD_SET_MESSAGE_INTERVAL, named, float(us))
        msgs = _collect(link, 1.0)
        acks = _acks(msgs, mav.MAV_CMD_SET_MESSAGE_INTERVAL)
        assert acks and acks[0].result == mav.MAV_RESULT_ACCEPTED, (
            f"SET_MESSAGE_INTERVAL {us} was not ACCEPTED"
        )

    assert housekeeping_interval() == -1, (
        "housekeeping reports an interval before anything armed it (is a stream left armed?)"
    )
    try:
        set_interval(2_000_000)
        assert housekeeping_interval() == 2_000_000
        set_interval(-1)
        assert housekeeping_interval() == -1
    finally:
        # Arming lasts until reset; leave it as found.
        _send(link, mav.MAV_CMD_SET_MESSAGE_INTERVAL, named, -1.0)
        _collect(link, 0.5)


# --- PROTOCOL_VERSION ---------------------------------------------------------------


def _decode_protocol_version(frame):
    """(version, min_version, max_version, spec_hash, library_hash) from a raw v2 frame.

    Decoded by hand because the harness's dialect does not define the message. A
    MAVLink 2 sender drops trailing zero bytes of the payload, and this message's
    end in zeros, so the payload is padded back to its 22 bytes first.
    """
    assert frame[0] == 0xFD, f"PROTOCOL_VERSION arrived as a v1 frame (STX {frame[0]:#x})"
    length = frame[1]
    payload = bytes(frame[10:10 + length]).ljust(22, b"\x00")
    return struct.unpack("<HHH8s8s", payload)


def test_protocol_version_is_sent_on_request(link):
    mav = _mavlink()
    msgs = _exchange(link, mav.MAV_CMD_REQUEST_MESSAGE, float(PROTOCOL_VERSION_ID))

    versions = _of(msgs, _name_of(PROTOCOL_VERSION_ID))
    assert versions, "no PROTOCOL_VERSION after MAV_CMD_REQUEST_MESSAGE for id 300"
    version, min_version, max_version, spec_hash, library_hash = _decode_protocol_version(
        versions[0].get_msgbuf()
    )
    assert (version, min_version, max_version) == (200, 200, 200), (
        f"version/min/max = {version}/{min_version}/{max_version}, expected 200/200/200"
    )
    assert not any(spec_hash), "spec_version_hash is not zero"
    assert not any(library_hash), "library_version_hash is not zero"

    acks = _acks(msgs, mav.MAV_CMD_REQUEST_MESSAGE)
    assert acks and acks[0].result == mav.MAV_RESULT_ACCEPTED, "no ACCEPTED COMMAND_ACK"


def test_protocol_version_is_not_sent_unasked(link):
    """link.sample() is the 12 s listen taken before any case ran, so nothing here asked."""
    counts = link.sample().counts
    assert _name_of(PROTOCOL_VERSION_ID) not in counts and "PROTOCOL_VERSION" not in counts, (
        "PROTOCOL_VERSION appeared on the link with nobody having requested it"
    )


# --- MISSION_REQUEST_LIST -----------------------------------------------------------


def test_mission_request_list_is_answered_with_an_empty_mission(link):
    mav = _mavlink()
    for mission_type in (mav.MAV_MISSION_TYPE_MISSION, mav.MAV_MISSION_TYPE_FENCE):
        _drain(link)
        link.mav.mav.mission_request_list_send(1, mav.MAV_COMP_ID_AUTOPILOT1, mission_type)
        msgs = _collect(link, 2.0)

        counts = _of(msgs, "MISSION_COUNT")
        assert counts, f"no MISSION_COUNT for a MISSION_REQUEST_LIST of type {mission_type}"
        c = counts[0]
        assert c.count == 0, f"count {c.count}, expected 0"
        assert c.mission_type == mission_type, f"mission_type {c.mission_type}, expected {mission_type}"
        # The harness is system 255, component 190 (hil.Link).
        assert (c.target_system, c.target_component) == (255, 190), (
            f"addressed to {c.target_system}/{c.target_component}, expected the sender, 255/190"
        )
        assert not _of(msgs, "STATUSTEXT"), "a STATUSTEXT followed a MISSION_REQUEST_LIST"


# --- Reduced configuration (manual) -------------------------------------------------


def test_battery_status_requested_in_the_reduced_configuration(link):
    """No battery is read in the reduced configuration, so BATTERY_STATUS is neither
    streamed nor sent on request. Needs the board put there by hand: see
    _require_reduced()."""
    _require_reduced(link)
    mav = _mavlink()

    msgs = _exchange(link, mav.MAV_CMD_REQUEST_MESSAGE, float(mav.MAVLINK_MSG_ID_BATTERY_STATUS), 2.5)
    acks = _acks(msgs, mav.MAV_CMD_REQUEST_MESSAGE)
    assert acks, "no COMMAND_ACK"
    assert acks[0].result == mav.MAV_RESULT_TEMPORARILY_REJECTED, (
        f"expected MAV_RESULT_TEMPORARILY_REJECTED, got {acks[0].result}"
    )
    assert not _of(msgs, "BATTERY_STATUS"), "a BATTERY_STATUS was sent with no battery reading"

    _, intervals, _ = _interval_of(link, mav.MAVLINK_MSG_ID_BATTERY_STATUS)
    assert intervals and intervals[0].interval_us == -1, (
        "GET_MESSAGE_INTERVAL for BATTERY_STATUS did not report -1 in the reduced configuration"
    )
