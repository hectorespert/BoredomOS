"""Periodic telemetry and vehicle identity.

Covers the mavlink-link spec requirements "The MAVLink link is carried on the
hardware UART" and "The vehicle identity is unchanged by the move".
"""

from hil import assert_rate


def test_heartbeat_at_1hz(link):
    assert_rate(link.sample(), "HEARTBEAT", 1.0)


def test_system_time_at_1hz(link):
    assert_rate(link.sample(), "SYSTEM_TIME", 1.0)


def test_battery_status_every_2s(link):
    assert_rate(link.sample(), "BATTERY_STATUS", 0.5)


def test_one_vehicle_with_the_expected_identity(link):
    from pymavlink import mavutil

    identities = link.sample().identities
    assert identities == {(1, mavutil.mavlink.MAV_COMP_ID_AUTOPILOT1)}, (
        f"expected exactly system 1 / MAV_COMP_ID_AUTOPILOT1, saw {sorted(identities)}"
    )


def test_vehicle_type_is_rocket(link):
    from pymavlink import mavutil

    types = link.sample().types
    expected = mavutil.mavlink.MAV_TYPE_ROCKET
    assert types == {expected}, f"expected MAV_TYPE_ROCKET ({expected}), saw {types}"


def test_link_carries_no_garbage(link):
    """BAD_DATA means bytes that are not MAVLink, which on a dedicated link
    should not happen. A few can appear if we connected mid-frame, so allow one."""
    bad = link.sample().counts.get("BAD_DATA", 0)
    assert bad <= 1, f"{bad} BAD_DATA blocks on the link"
