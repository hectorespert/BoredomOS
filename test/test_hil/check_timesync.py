"""TIMESYNC is answered, in the time base a ground station expects.

Covers the mavlink-link scenario about TIMESYNC and, since
improve-clock-synchronisation, the system-clock scenario "A ground station
measures the link offset": the reply carries monotonic time since boot in
nanoseconds, not wall clock. Both reference autopilots answer that way
(ArduPilot's timesync_receive_timestamp_ns, PX4's hrt_absolute_time * 1000), and
the dialect itself specifies no base for the field.

Before that change the reply carried getUnixTimeNsec() -- wall clock, quantised
to the second -- and this file asserted only tc1 > 0, which is exactly why the
wrong base went unnoticed.
"""

import time


def _request_timesync(link, timeout=5.0):
    ts1 = int(time.time() * 1e9)
    link.mav.mav.timesync_send(0, ts1)

    started = time.time()
    while time.time() - started < timeout:
        msg = link.mav.recv_match(type="TIMESYNC", blocking=True, timeout=1.0)
        if msg and msg.tc1 != 0:
            return ts1, msg
    return ts1, None


def test_timesync_is_answered(link):
    ts1, reply = _request_timesync(link)

    assert reply is not None, "TIMESYNC with tc1 == 0 was not answered"
    assert reply.ts1 == ts1, f"ts1 was not echoed back: sent {ts1}, got {reply.ts1}"
    assert reply.tc1 > 0, "reply carries no timestamp"


def test_timesync_answers_in_time_since_boot(link):
    """tc1 is elapsed time since boot, not a UNIX timestamp.

    A wall-clock answer is ~1.7e18 ns and climbing; time since boot on a board
    that has been up for minutes or hours is many orders of magnitude smaller.
    The gap is far too wide for this to be a flaky threshold.
    """
    _, reply = _request_timesync(link)
    assert reply is not None, "TIMESYNC was not answered"

    host_wall_ns = int(time.time() * 1e9)
    # Host-relative, so no uptime limit is implied. A wall-clock answer is within
    # a hair of host_wall_ns; time since boot would have to reach half the UNIX
    # epoch -- around 28 years -- to trip this, which is not a mission limit any
    # more than the hardware's own lifetime is. An earlier version of this check
    # used a fixed one-year constant, which Copilot's review pointed out was
    # exactly the uptime assumption the comment claimed not to make.
    assert reply.tc1 < host_wall_ns // 2, (
        f"tc1 looks like wall clock, not time since boot: {reply.tc1} "
        f"(host wall clock is {host_wall_ns})"
    )


def test_timesync_advances_by_the_time_waited(link):
    """Two requests separated by a known wait differ by roughly that wait."""
    _, first = _request_timesync(link)
    assert first is not None, "first TIMESYNC was not answered"

    waited = 3.0
    time.sleep(waited)

    _, second = _request_timesync(link)
    assert second is not None, "second TIMESYNC was not answered"

    advanced_s = (second.tc1 - first.tc1) / 1e9
    assert advanced_s > 0, f"tc1 did not advance: {first.tc1} then {second.tc1}"
    # Generous bounds: each request costs a round trip and the reply is stamped
    # on receipt, so the measured gap is the wait plus scheduling, never less
    # than the wait by more than one sampling period.
    assert waited - 1.0 <= advanced_s <= waited + 4.0, (
        f"tc1 advanced {advanced_s:.2f}s across a {waited:.0f}s wait"
    )
