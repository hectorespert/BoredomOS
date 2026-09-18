"""Housekeeping telemetry: off by default, armed by the ground, session-scoped.

Covers the mavlink-link spec requirement "Housekeeping telemetry is available
on request, never unsolicited"
(openspec/changes/add-mavlink-housekeeping-telemetry/specs/mavlink-link/spec.md).

**Order matters in this file.** run.py gives every case in a run the same
Link with no reset between them (run.py:8-10), and the armed/disarmed state
this file exercises is session-scoped on the board, not reset between
`run.py` invocations either -- it only clears on an actual board reset. Cases
that assert silence run first, before anything here sends
MAV_CMD_SET_MESSAGE_INTERVAL, and the last case that leaves the stream armed
(test_disable_stops_an_armed_stream) explicitly disables it before returning,
so whatever runs after this file -- in this run or the next one -- starts
from a clean, disabled state.

None of this touches link.sample()'s cached 12 s window: that capture already
happened, before any test in any module ran (run.py's own main() calls it
once at startup), so it reflects only pre-command boot traffic. Every check
here listens fresh instead, the same way check_clock.py and check_timesync.py
already do.
"""

import os
import time
from collections import Counter

from hil import NoLinkError

NAMED_VALUE_INT = "NAMED_VALUE_INT"

# The published set, in the cursor's own order (src/mavlink.cpp's
# kHousekeepingTasks table): 2 heap values, then every task that exists in
# the running configuration. "SerialWrite" truncates to "SerialWrit" in the
# 10-byte NAMED_VALUE_INT name field -- see that table's comment.
NORMAL_NAMES = [
    "HeapFree", "HeapMin",
    "SerialRead", "SerialWrit", "Mavlink", "Cli", "Logger", "SdWrite",
]
REDUCED_NAMES = [
    "HeapFree", "HeapMin",
    "SerialRead", "SerialWrit", "Mavlink", "Cli",
]


def _decode_name(raw):
    if isinstance(raw, bytes):
        raw = raw.rstrip(b"\x00").decode("ascii", errors="replace")
    return raw.rstrip("\x00")


def _arm(link, interval_us):
    from pymavlink import mavutil

    link.mav.mav.command_long_send(
        1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
        mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL, 0,
        float(mavutil.mavlink.MAVLINK_MSG_ID_NAMED_VALUE_INT), float(interval_us),
        0, 0, 0, 0, 0,
    )


def _wait_ack(link, timeout=5.0):
    from pymavlink import mavutil

    deadline = time.time() + timeout
    while time.time() < deadline:
        msg = link.mav.recv_match(
            type="COMMAND_ACK", blocking=True, timeout=max(0.1, deadline - time.time())
        )
        if msg is not None and msg.command == mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL:
            return msg
    return None


def _listen(link, seconds):
    """A fresh window of traffic, independent of link.sample()'s one-shot
    cache -- see this module's docstring."""
    counts = Counter()
    started = time.time()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(blocking=True, timeout=1.0)
        if msg is not None:
            counts[msg.get_type()] += 1
    return counts


def _current_config(link):
    """Returns "normal" or "reduced", decoded from the next HEARTBEAT's
    system_status -- the same field check_recovery.py's state-decode cases
    use."""
    from pymavlink import mavutil

    heartbeat = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=5.0)
    if heartbeat is None:
        raise NoLinkError("no HEARTBEAT to read the current configuration from")
    if heartbeat.system_status == mavutil.mavlink.MAV_STATE_CRITICAL:
        return "reduced"
    return "normal"


def test_no_request_means_silence(link):
    """No case above this one in module order sends
    MAV_CMD_SET_MESSAGE_INTERVAL, so the stream is still in its clean-boot,
    off state here."""
    counts = _listen(link, 5.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, (
        f"{counts[NAMED_VALUE_INT]} NAMED_VALUE_INT with nothing requested"
    )


def test_interval_below_the_floor_is_denied(link):
    from pymavlink import mavutil

    _arm(link, 500_000)  # 500 ms -- below the 1000 ms floor
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK for a below-floor request"
    assert ack.result == mavutil.mavlink.MAV_RESULT_DENIED, (
        f"expected MAV_RESULT_DENIED, got {ack.result}"
    )

    counts = _listen(link, 3.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, (
        "a denied request must not start the stream"
    )


def test_default_rate_from_clean_boot_does_not_start_the_stream(link):
    from pymavlink import mavutil

    _arm(link, 0)
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK for a param2 == 0 request"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED, got {ack.result}"
    )

    counts = _listen(link, 3.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, (
        "the default rate is off; it must not start the stream"
    )


def test_arming_covers_the_full_cycle_without_disturbing_existing_telemetry(link):
    from pymavlink import mavutil

    config = _current_config(link)
    expected = NORMAL_NAMES if config == "normal" else REDUCED_NAMES

    _arm(link, 1_000_000)  # 1000 ms -- at the floor
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK for a 1000 ms request"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED, got {ack.result}"
    )

    # One full cycle at 1000 ms takes len(expected) seconds; double it for
    # margin and to also get a fair rate estimate for the other entries.
    seconds = max(12.0, len(expected) * 2.0)
    started = time.time()
    names_seen = []
    counts = Counter()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(blocking=True, timeout=1.0)
        if msg is None:
            continue
        counts[msg.get_type()] += 1
        if msg.get_type() == NAMED_VALUE_INT:
            names_seen.append(_decode_name(msg.name))
    elapsed = time.time() - started

    assert names_seen, "no NAMED_VALUE_INT arrived while armed"
    # Every name observed must be one this configuration can publish, in the
    # cursor's own cycle order, repeating -- an implementation that repeats
    # one value, uses the wrong units, or never reaches a task boundary fails
    # here, not just on the first message.
    unexpected = set(names_seen) - set(expected)
    assert not unexpected, f"unexpected NAMED_VALUE_INT name(s): {sorted(unexpected)}"
    cycle_len = len(expected)
    start = expected.index(names_seen[0])
    rotated = expected[start:] + expected[:start]
    full_cycles = len(names_seen) // cycle_len
    assert full_cycles >= 1, (
        f"only {len(names_seen)} of {cycle_len} values seen in {elapsed:.1f}s"
    )
    for i in range(full_cycles * cycle_len):
        assert names_seen[i] == rotated[i % cycle_len], (
            f"cycle order broken at position {i}: "
            f"expected {rotated[i % cycle_len]}, got {names_seen[i]}"
        )

    for message, hz in (("HEARTBEAT", 1.0), ("SYSTEM_TIME", 1.0)):
        rate = counts.get(message, 0) / elapsed
        assert 0.75 <= rate <= 1.25, (
            f"{message} at {rate:.2f} Hz while housekeeping is armed, expected ~1 Hz"
        )
    if config == "normal":
        rate = counts.get("BATTERY_STATUS", 0) / elapsed
        assert 0.35 <= rate <= 0.65, (
            f"BATTERY_STATUS at {rate:.2f} Hz while housekeeping is armed, expected ~0.5 Hz"
        )


def test_default_rate_stops_an_already_armed_stream(link):
    """Continues from the previous case, which left the stream armed."""
    from pymavlink import mavutil

    _arm(link, 0)
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK for a param2 == 0 request"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED, got {ack.result}"
    )

    counts = _listen(link, 3.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, (
        "param2 == 0 must stop an already-armed stream, not just decline to start one"
    )


def test_disable_stops_an_armed_stream(link):
    """Re-arms briefly, then disables with -1 -- the spec's own disable
    scenario, and the case that restores a clean, disabled state for
    whatever runs after this file (see this module's docstring)."""
    from pymavlink import mavutil

    _arm(link, 1_000_000)
    assert _wait_ack(link) is not None, "no COMMAND_ACK for the arming request"

    _arm(link, -1_000_000)  # param2 == -1 in microseconds
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK for a -1 (disable) request"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED, got {ack.result}"
    )

    counts = _listen(link, 3.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, "stream kept sending after -1"


def test_no_housekeeping_survives_a_reset(link):
    """Needs hands: skipped unless HIL_MANUAL_RESET=1, since run.py otherwise
    expects every case to finish unattended (run.py's own docstring). When
    run, arms housekeeping, then pauses for a human to press the board's
    **RESET button** -- never a 1200-baud touch: README.md:86-87 says this
    board enters DFU and stays there on one, unlike AVR boards, and using it
    here would strand the board and need a manual reflash to recover.
    """
    from pymavlink import mavutil

    if os.environ.get("HIL_MANUAL_RESET") != "1":
        raise NoLinkError(
            "manual case: set HIL_MANUAL_RESET=1 and be ready to press the "
            "board's RESET button when prompted to run it"
        )

    _arm(link, 1_000_000)
    ack = _wait_ack(link)
    assert ack is not None and ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        "arming before the reset was not accepted"
    )
    counts = _listen(link, 3.0)
    assert counts.get(NAMED_VALUE_INT, 0) > 0, "housekeeping never started before the reset"

    input(
        "\nPress the board's RESET button now (NOT a 1200-baud touch -- "
        "test/test_hil/README.md:86-87) and press Enter here once it has "
        "rebooted: "
    )

    heartbeat = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=15.0)
    assert heartbeat is not None, "no HEARTBEAT after the reset"

    counts = _listen(link, 5.0)
    assert counts.get(NAMED_VALUE_INT, 0) == 0, (
        "NAMED_VALUE_INT arrived after a reset without a fresh request"
    )


def test_housekeeping_in_the_reduced_configuration(link):
    """check_recovery.py has no setup for entering the reduced
    configuration -- its cases only assert against a board already in that
    state, and self-skip otherwise (check_recovery.py:45-53). This case does
    the same rather than assuming a fixture that does not exist.

    **Manual precondition** (pick one, per fault-recovery's spec): physically
    remove the SD card and reset the board, which is the faster and more
    repeatable of the two reduced-entry paths -- forcing three consecutive
    failed boots also works but is slower to set up by hand. **Restore**:
    reinsert the card and reset again once this case has run.
    """
    config = _current_config(link)
    if config != "reduced":
        raise NoLinkError(
            "board is not in the reduced configuration -- see this case's "
            "docstring for how to put it there"
        )

    from pymavlink import mavutil

    _arm(link, 1_000_000)
    ack = _wait_ack(link)
    assert ack is not None, "no COMMAND_ACK in the reduced configuration"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED in the reduced configuration, got {ack.result}"
    )

    seconds = max(12.0, len(REDUCED_NAMES) * 2.0)
    names_seen = []
    started = time.time()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(type=NAMED_VALUE_INT, blocking=True, timeout=1.0)
        if msg is not None:
            names_seen.append(_decode_name(msg.name))
    assert names_seen, "no NAMED_VALUE_INT arrived while armed in the reduced configuration"
    unexpected = set(names_seen) - set(REDUCED_NAMES)
    assert not unexpected, (
        f"unexpected NAMED_VALUE_INT name(s) in the reduced configuration: {sorted(unexpected)}"
    )

    _arm(link, -1_000_000)
    _wait_ack(link)
