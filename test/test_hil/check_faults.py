"""The fault hooks, driven by an injection build.

Covers the fault-recovery scenario "A task overflows its stack", the memory-budget
scenarios "An allocation fails with nothing attached" and "An allocation fails before
any task has started", and the on-request half of "The boot statement names the
faulting task" (openspec/changes/archive/2026-09-27-report-the-faulting-task/).

**Needs a build nobody flies.** The firmware under test must have been flashed with
`PLATFORMIO_BUILD_FLAGS='-D INJECT_FAULT=<n>' pio run -t upload`, and
`HIL_FAULT_INJECTED=<n>` must say which: without it the case self-skips, since
the normal firmware has no fault to report. See README.md for the three variants and
for putting the board back afterwards.

No hands needed, and USB is enough: the case clears the counters with
MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, which arms the injection (it fires only on a boot
that started with no consecutive fault), waits for the fault's reset, reopens the port
and asks for the boot statement with MAV_CMD_REQUEST_MESSAGE rather than waiting for
the unsolicited one, which USB cannot observe across a reset.
"""

import os
import time

from hil import NoLinkError

STACK_OVERFLOW_PHASE = 7  # Recovery::BootPhase::StackOverflowFault -- include/Recovery.h
MALLOC_FAILED_PHASE = 8  # Recovery::BootPhase::MallocFailedFault

# What each INJECT_FAULT variant should leave behind: the phase byte in custom_mode
# and the boot statement. The reset is a software one, from NVIC_SystemReset().
EXPECTED = {
    "1": (STACK_OVERFLOW_PHASE, "Reset: software, overflow Logger"),
    "2": (MALLOC_FAILED_PHASE, "Reset: software, malloc Logger"),
    "3": (MALLOC_FAILED_PHASE, "Reset: software, malloc setup"),
}


def _reconnect(link):
    """Reopen the port and hand the connection to the shared link object.

    run.py passes one Link to every case, so a reconnection kept local would leave
    every later case talking to a dead handle -- as check_clock.py found.
    """
    from hil import Link

    link.close()
    fresh = Link()
    link.mav = fresh.mav
    link.port = fresh.port
    link.baud = fresh.baud
    link._sample = None  # pylint: disable=protected-access


def _wait_for_fault_heartbeat(link, phase, seconds=90.0):
    """The first HEARTBEAT whose phase byte is `phase`, reconnecting across resets."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            hb = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=2.0)
        except Exception:  # pylint: disable=broad-except
            hb = None
        if hb is not None and (hb.custom_mode >> 8) & 0xFF == phase:
            return hb
        if hb is None:
            # The port dropped with the reset, or has not come back yet.
            time.sleep(1.0)
            try:
                _reconnect(link)
            except NoLinkError:
                pass
    return None


def test_an_injected_fault_is_reported_with_its_task(link):
    from pymavlink import mavutil

    variant = os.environ.get("HIL_FAULT_INJECTED")
    if variant not in EXPECTED:
        raise NoLinkError(
            "needs an injection build: flash with PLATFORMIO_BUILD_FLAGS='-D INJECT_FAULT=<n>' "
            "and set HIL_FAULT_INJECTED=<n> (1, 2 or 3) -- see README.md"
        )
    phase, statement = EXPECTED[variant]
    mav = mavutil.mavlink

    # Clears both counts and restarts: the boot that follows starts at consecutive 0,
    # which is what arms the injection.
    link.mav.mav.command_long_send(
        1, mav.MAV_COMP_ID_AUTOPILOT1, mav.MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 0,
        1, 0, 0, 0, 0, 0, 0,
    )
    ack = link.mav.recv_match(type="COMMAND_ACK", blocking=True, timeout=5.0)
    assert ack is not None, "the reboot command was not acknowledged"

    time.sleep(3.0)
    hb = _wait_for_fault_heartbeat(link, phase)
    assert hb is not None, (
        f"no HEARTBEAT with phase {phase} within 90 s of the reboot: the injected fault "
        "did not fire, or the board did not report it"
    )
    consecutive = (hb.custom_mode >> 16) & 0xFF
    assert consecutive >= 1, f"the fault reset did not advance the consecutive count ({consecutive})"

    while link.mav.recv_match(blocking=False) is not None:
        pass
    link.mav.mav.command_long_send(
        1, mav.MAV_COMP_ID_AUTOPILOT1, mav.MAV_CMD_REQUEST_MESSAGE, 0,
        253.0, 0, 0, 0, 0, 0, 0,
    )
    text = None
    deadline = time.time() + 3.0
    while time.time() < deadline and text is None:
        msg = link.mav.recv_match(type="STATUSTEXT", blocking=True, timeout=1.0)
        if msg is not None and msg.text.startswith("Reset: "):
            text = msg.text
    assert text == statement, f"boot statement {text!r}, expected {statement!r}"
