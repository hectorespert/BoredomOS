"""Reduced-configuration heartbeat state and the closed message set.

Covers the fault-recovery spec requirement "The ground can see the state
without asking for it" (test_first_heartbeat_reports_reduced_state,
test_heartbeat_reports_operational) and the message-set clause of "Normal
operation is not reported as degraded" (test_observed_message_set_is_closed).

Both state-decode cases must self-skip on a board not in the state they check
-- asserting the reduced state would FAIL a healthy board and turn a routine
`pio test` red. They raise hil.NoLinkError instead, which run.py reports as
IGNORE, indistinguishable from "no board" -- see test-plan.md's note on this.

**Known gap, not resolved here.** run.py's own main() calls link.sample() once
at startup to confirm the link works before running any case, which consumes
several seconds of traffic before any test function runs. The first heartbeat
this module can decode is therefore not literally the first one since the link
opened -- test-plan.md's TP-14 "no host-originated frame before it" clause is
not fully evidenced through this harness as it stands. See review.md's
test-plan audit (b)5 and design.md.
"""

from hil import NoLinkError

REASON_MAX = 5  # Recovery::ResetReason's highest value -- include/Recovery.h
PHASE_MAX = 8  # Recovery::BootPhase's highest value -- include/Recovery.h


def _decode_custom_mode(heartbeat):
    mode = heartbeat.custom_mode
    return {
        "reason": mode & 0xFF,
        "phase": (mode >> 8) & 0xFF,
        "consecutive": (mode >> 16) & 0xFF,
        "cumulative": (mode >> 24) & 0xFF,
    }


def _next_heartbeat(link, timeout=15.0):
    msg = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=timeout)
    if msg is None:
        raise NoLinkError("no HEARTBEAT seen")
    return msg


def test_first_heartbeat_reports_reduced_state(link):
    from pymavlink import mavutil

    heartbeat = _next_heartbeat(link)
    if heartbeat.system_status != mavutil.mavlink.MAV_STATE_CRITICAL:
        raise NoLinkError("board is not in the reduced configuration")

    assert not (heartbeat.base_mode & mavutil.mavlink.MAV_MODE_FLAG_AUTO_ENABLED), (
        "reduced heartbeat still advertises MAV_MODE_FLAG_AUTO_ENABLED"
    )
    fields = _decode_custom_mode(heartbeat)
    assert fields["reason"] <= REASON_MAX, f"reason byte {fields['reason']} out of range"
    assert fields["phase"] <= PHASE_MAX, f"phase byte {fields['phase']} out of range"


def test_heartbeat_reports_operational(link):
    from pymavlink import mavutil

    heartbeat = _next_heartbeat(link)
    if heartbeat.system_status != mavutil.mavlink.MAV_STATE_ACTIVE:
        raise NoLinkError("board is not in the normal configuration")

    assert heartbeat.base_mode & mavutil.mavlink.MAV_MODE_FLAG_AUTO_ENABLED, (
        "operational heartbeat is missing MAV_MODE_FLAG_AUTO_ENABLED"
    )


def test_observed_message_set_is_closed(link):
    """The shared 12 s sample must contain only message types this firmware
    sends. COMMAND_ACK is allowed unconditionally: it only appears once a
    MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN has been sent during the run (task 8.1),
    which this case has no way to know happened. BAD_DATA is allowed too --
    check_telemetry.py's test_link_carries_no_garbage already bounds how much
    of it is tolerated; this case only cares which types appear, not how many.
    """
    expected = {"HEARTBEAT", "SYSTEM_TIME", "BATTERY_STATUS", "COMMAND_ACK", "BAD_DATA"}
    seen = set(link.sample().counts.keys())
    unexpected = seen - expected
    assert not unexpected, f"unexpected message types on the link: {sorted(unexpected)}"
