"""The clock can be set from the ground, and reports what it knows.

Covers the mavlink-link scenario "inbound SYSTEM_TIME and TIMESYNC sent on that
port are acted upon" and, since improve-clock-synchronisation, the system-clock
requirements about sub-second resolution, validation of what arrives from the
ground, and the provenance reported at boot. Note this WRITES to the satellite's
DS1307.

Two system-clock requirements are NOT covered here and cannot be: reporting an
unknown clock as zero, and the origin being none, both need the DS1307 absent.
It is not disconnectable on this board -- see the change's proposal.md, Impact.
"""

import time


def _read_unix_time(link, seconds=3.0):
    got = None
    started = time.time()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(type="SYSTEM_TIME", blocking=True, timeout=1.0)
        if msg:
            got = msg.time_unix_usec // 1_000_000
    assert got is not None, "no SYSTEM_TIME received"
    return got


def _collect_system_time(link, seconds=4.0):
    samples = []
    started = time.time()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(type="SYSTEM_TIME", blocking=True, timeout=1.0)
        if msg:
            samples.append(msg.time_unix_usec)
    assert samples, "no SYSTEM_TIME received"
    return samples


def _statustext_matching(link, needle, seconds=6.0):
    started = time.time()
    while time.time() - started < seconds:
        msg = link.mav.recv_match(type="STATUSTEXT", blocking=True, timeout=1.0)
        if msg:
            text = msg.text.decode() if isinstance(msg.text, bytes) else msg.text
            if needle in text:
                return text
    return None


def test_inbound_system_time_sets_the_clock(link):
    host = int(time.time())
    link.mav.mav.system_time_send(host * 1_000_000, 0)
    time.sleep(2.5)
    after = _read_unix_time(link)
    drift = abs(after - int(time.time()))
    assert drift <= 2, f"clock is {drift}s off after being set from the ground"


def test_reported_time_has_a_sub_second_part(link):
    """system-clock: reported UNIX time resolves below one second.

    Before improve-clock-synchronisation the microsecond field was the second
    multiplied by 10^6, so every sample landed exactly on a second.
    """
    samples = _collect_system_time(link)
    fractions = [s % 1_000_000 for s in samples]
    assert any(f != 0 for f in fractions), (
        f"every SYSTEM_TIME landed exactly on a second: {fractions}"
    )


def test_reported_time_never_goes_backwards(link):
    """system-clock: successive readings never decrease.

    This is what a R64CNT read without the carry retry would break: a stale
    second paired with a fresh fraction steps back by up to a second.
    """
    samples = _collect_system_time(link, seconds=8.0)
    for previous, nxt in zip(samples, samples[1:]):
        assert nxt >= previous, f"SYSTEM_TIME went backwards: {previous} then {nxt}"


def test_an_implausible_time_is_refused(link):
    """system-clock: a time offered by the ground is accepted only if plausible."""
    before = _read_unix_time(link)

    # Zero, and one second before the 2022-01-01 floor.
    link.mav.mav.system_time_send(0, 0)
    time.sleep(1.0)
    link.mav.mav.system_time_send(1_640_995_199 * 1_000_000, 0)
    time.sleep(2.0)

    after = _read_unix_time(link)
    assert after >= before, f"clock moved backwards after a refused time: {before} -> {after}"
    assert after - before < 30, (
        f"clock jumped {after - before}s after a time it should have refused"
    )


def test_a_refused_time_emits_nothing(link):
    """mavlink-link: a peer repeating a rejected value generates no traffic.

    A STATUSTEXT per rejection would let a remote peer flood the write queue.
    """
    # Drain whatever is queued, then hammer the port with refusable times.
    link.mav.recv_match(type="STATUSTEXT", blocking=False)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        link.mav.mav.system_time_send(0, 0)
        time.sleep(0.02)

    text = _statustext_matching(link, "Clock:", seconds=3.0)
    assert text is None, f"a refused time produced a STATUSTEXT: {text!r}"


def test_the_clock_survives_a_commanded_restart(link):
    """system-clock: a clock set from the ground outlives a reset.

    Gated behind HIL_CLOCK_RESET=1 because it reboots the board mid-run, which
    would disturb every case after it -- the same reason check_housekeeping.py
    gates its reset case.

    No hands needed: the firmware answers MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN. The
    DS1307 stays attached, so this does NOT observe the `survived` origin (ds1307
    outranks it and wins the seeding). What it observes is the "found live" fact
    the boot report carries separately for exactly this reason -- see the change's
    design.md.
    """
    import os

    from hil import NoLinkError

    if os.environ.get("HIL_CLOCK_RESET") != "1":
        raise NoLinkError(
            "disruptive case: set HIL_CLOCK_RESET=1 to let it reboot the board"
        )

    if link.is_usb_cdc:
        raise NoLinkError(
            "the boot report cannot be observed over USB CDC: the port drops when "
            "the board resets and the firmware, which does not wait for a host, "
            "writes the text into a port nobody is holding. Point HIL_PORT at a "
            "USB-TTL adapter on D0/D1 to run this"
        )

    from pymavlink import mavutil

    host = int(time.time())
    link.mav.mav.system_time_send(host * 1_000_000, 0)
    time.sleep(2.0)
    before = _read_unix_time(link)

    link.mav.mav.command_long_send(
        1,
        mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
        mavutil.mavlink.MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN,
        0,
        1, 0, 0, 0, 0, 0, 0,
    )

    ack = link.mav.recv_match(type="COMMAND_ACK", blocking=True, timeout=5.0)
    assert ack is not None, "the reboot command was not acknowledged"

    # Reconnect rather than reusing the handle, and hand the new connection back
    # to the SHARED link object: run.py passes one Link to every case and closes it
    # after the loop, so leaving the reconnection local here would leave every
    # later case talking to a dead handle. Copilot's review of this change caught
    # that.
    time.sleep(3.0)
    from hil import Link

    try:
        fresh = Link()
    except NoLinkError as exc:
        raise NoLinkError(f"the board did not come back after the reboot: {exc}") from exc

    link.mav = fresh.mav
    link.port = fresh.port
    link.baud = fresh.baud
    link._sample = None  # pylint: disable=protected-access

    text = None
    started = time.time()
    while time.time() - started < 20.0:
        msg = link.mav.recv_match(type="STATUSTEXT", blocking=True, timeout=2.0)
        if msg:
            body = msg.text.decode() if isinstance(msg.text, bytes) else msg.text
            if body.startswith("Clock:"):
                text = body
                break

    assert text is not None, "no clock report after the restart"
    assert "found live" in text, (
        f"the internal RTC did not keep running across the restart: {text!r}"
    )

    after = _read_unix_time(link)
    assert after != 0, "the clock came back as unknown"
    assert after >= before - 2, f"time went backwards across the restart: {before} -> {after}"


def test_a_clock_set_reports_its_new_origin(link):
    """system-clock: the ground supersedes the battery-backed clock."""
    host = int(time.time())
    link.mav.mav.system_time_send(host * 1_000_000, 0)

    text = _statustext_matching(link, "Clock:")
    # Only emitted when the origin actually changes, so a board already on
    # "ground" from an earlier case in the same run stays silent -- which is
    # correct behaviour, not a failure.
    if text is not None:
        assert "ground" in text, f"origin not reported as ground: {text!r}"
