"""AUTOPILOT_VERSION: answered on request, honestly.

Covers the mavlink-link spec requirement "MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES
is answered"
(openspec/changes/answer-autopilot-version-requests/specs/mavlink-link/spec.md).
The per-port scenario ("Requested on one port does not answer on the other")
needs both ports at once and lives in check_dual_link.py instead, alongside
its siblings for other per-port claims.

This does not touch link.sample()'s cached 12 s window (that capture already
happened, before any test in any module ran) -- listens fresh instead, the
same way check_clock.py, check_timesync.py and check_housekeeping.py do.
"""

import time

AUTOPILOT_VERSION = "AUTOPILOT_VERSION"

# The MAVLink wire protocol's own magic bytes: 0xFE starts a v1 frame, 0xFD a
# v2 one (mavlink/mavlink_types.h upstream; this project's src/mavlink.cpp
# never calls mavlink_set_proto_version(), so both ports emit v2
# unconditionally -- design.md Decision 2, answer-autopilot-version-requests).
MAVLINK_STX_V2 = 0xFD


def _request(link):
    from pymavlink import mavutil

    link.mav.mav.command_long_send(
        1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
        mavutil.mavlink.MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES, 0,
        1, 0, 0, 0, 0, 0, 0,  # param1: request autopilot version (MAV_BOOL_TRUE)
    )


def _wait_ack(link, command, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        msg = link.mav.recv_match(
            type="COMMAND_ACK", blocking=True, timeout=max(0.1, deadline - time.time())
        )
        if msg is not None and msg.command == command:
            return msg
    return None


def test_capabilities_request_is_answered(link):
    from pymavlink import mavutil

    _request(link)

    version = link.mav.recv_match(type=AUTOPILOT_VERSION, blocking=True, timeout=5.0)
    assert version is not None, "no AUTOPILOT_VERSION after MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES"

    # Only the one true capability this firmware has -- design.md Decision 2.
    # No mission, parameter or FTP bit; nothing this firmware does not answer.
    assert version.capabilities == mavutil.mavlink.MAV_PROTOCOL_CAPABILITY_MAVLINK2, (
        f"capabilities = {version.capabilities:#x}, expected only "
        f"MAV_PROTOCOL_CAPABILITY_MAVLINK2 ({mavutil.mavlink.MAV_PROTOCOL_CAPABILITY_MAVLINK2:#x})"
    )

    # The claim is also true on the wire, not just in the field the firmware
    # reports about itself -- design.md Decision 1a/2 names this as the live
    # confirmation source-reading alone cannot give.
    stx = version.get_msgbuf()[0]
    assert stx == MAVLINK_STX_V2, (
        f"AUTOPILOT_VERSION arrived as a v1 frame (STX {stx:#x}); "
        "capabilities claims MAVLINK2 but the wire disagrees"
    )

    ack = _wait_ack(link, mavutil.mavlink.MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES)
    assert ack is not None, "no COMMAND_ACK for MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES"
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, (
        f"expected MAV_RESULT_ACCEPTED, got {ack.result}"
    )
