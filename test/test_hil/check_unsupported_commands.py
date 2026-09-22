"""COMMAND_LONG always gets a COMMAND_ACK, even when this firmware cannot act on it.

Covers the mavlink-link spec requirement "Every COMMAND_LONG receives a
COMMAND_ACK"
(openspec/changes/answer-unsupported-command-long-requests/specs/mavlink-link/spec.md).
Before that change, a `command.command` this firmware did not recognise --
including MAV_CMD_GET_HOME_POSITION, which was checked but never
acknowledged -- and a MAV_CMD_SET_MESSAGE_INTERVAL naming any message id
other than NAMED_VALUE_INT (252) both fell through with no reply at all.

Listens fresh for each case rather than touching link.sample()'s cached
window, the same way check_capabilities.py does.
"""

import time

# Any MAV_CMD this firmware's COMMAND_LONG sub-switch never checks against.
# Deliberately not one of the four it does handle (REQUEST_AUTOPILOT_CAPABILITIES,
# PREFLIGHT_REBOOT_SHUTDOWN, SET_MESSAGE_INTERVAL, GET_HOME_POSITION).
UNRECOGNISED_COMMAND = "MAV_CMD_DO_SET_MODE"


def _send_command(link, command_name, params=(0, 0, 0, 0, 0, 0, 0)):
    from pymavlink import mavutil

    link.mav.mav.command_long_send(
        1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1,
        getattr(mavutil.mavlink, command_name), 0,
        *params,
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


def test_unrecognised_command_is_denied_unsupported(link):
    from pymavlink import mavutil

    _send_command(link, UNRECOGNISED_COMMAND)

    command_id = getattr(mavutil.mavlink, UNRECOGNISED_COMMAND)
    ack = _wait_ack(link, command_id)
    assert ack is not None, f"no COMMAND_ACK for {UNRECOGNISED_COMMAND}"
    assert ack.result == mavutil.mavlink.MAV_RESULT_UNSUPPORTED, (
        f"expected MAV_RESULT_UNSUPPORTED, got {ack.result}"
    )


def test_get_home_position_is_answered_unsupported(link):
    from pymavlink import mavutil

    _send_command(link, "MAV_CMD_GET_HOME_POSITION")

    ack = _wait_ack(link, mavutil.mavlink.MAV_CMD_GET_HOME_POSITION)
    assert ack is not None, "no COMMAND_ACK for MAV_CMD_GET_HOME_POSITION"
    assert ack.result == mavutil.mavlink.MAV_RESULT_UNSUPPORTED, (
        f"expected MAV_RESULT_UNSUPPORTED, got {ack.result}"
    )


def test_set_message_interval_unsupported_msgid_is_denied(link):
    from pymavlink import mavutil

    # param1: HEARTBEAT's own message id -- a real, existing message this
    # firmware sends periodically, but not one the ground may reschedule via
    # SET_MESSAGE_INTERVAL (only NAMED_VALUE_INT/252 is). param2: 1 Hz in
    # microseconds -- a well-formed, in-range interval, so a DENIED result can
    # only be explained by the message id, not by the rate.
    _send_command(
        link, "MAV_CMD_SET_MESSAGE_INTERVAL",
        params=(mavutil.mavlink.MAVLINK_MSG_ID_HEARTBEAT, 1_000_000, 0, 0, 0, 0, 0),
    )

    ack = _wait_ack(link, mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL)
    assert ack is not None, "no COMMAND_ACK for MAV_CMD_SET_MESSAGE_INTERVAL"
    assert ack.result == mavutil.mavlink.MAV_RESULT_DENIED, (
        f"expected MAV_RESULT_DENIED, got {ack.result}"
    )
