"""SYS_STATUS: sensor presence, battery state and link error counters.

Covers the mavlink-link spec requirement "SYS_STATUS reports sensor
presence, battery state and link error counters every second"
(openspec/specs/mavlink-link/spec.md) and the fault-recovery spec's "A
ground station connects after the card was found absent" scenario, to the
extent a bench with the SD card present can (the card-absent side needs the
card physically pulled -- see the archived
openspec/changes/archive/2026-09-22-emit-sys-status/tasks.md, 3.2).

Cadence uses link.sample()'s cached 12 s window, like check_telemetry.py.
Field-content cases listen fresh, the same way check_capabilities.py does.
"""

import time

from hil import assert_rate


def test_sys_status_at_1hz(link):
    assert_rate(link.sample(), "SYS_STATUS", 1.0)


def test_sd_presence_is_visible_without_asking(link):
    """fault-recovery spec, "Absent hardware degrades rather than halts":
    a ground station connecting after boot sees the SD card's presence in
    SYS_STATUS with no MAV_CMD sent first -- the same "state without asking
    for it" guarantee the heartbeat already gives the reduced configuration.

    Known gap, shared with check_recovery.py's test_observed_message_set_is_
    closed: run.py's own main() already consumes several seconds of traffic
    via link.sample() before any case runs, so this is not literally the
    first SYS_STATUS since the link opened -- it only demonstrates that no
    request is sent, not that none was ever needed since power-on.
    """
    from pymavlink import mavutil

    status = link.mav.recv_match(type="SYS_STATUS", blocking=True, timeout=2.0)
    assert status is not None, "no SYS_STATUS received without requesting one"
    assert status.onboard_control_sensors_present & mavutil.mavlink.MAV_SYS_STATUS_LOGGING, (
        "SD card present but LOGGING not set on an unrequested SYS_STATUS"
    )


def test_sd_card_present_sets_the_logging_bit(link):
    """Bench configuration: the SD card is present. MAV_SYS_STATUS_LOGGING
    is expected set in all three bitmaps, since this firmware mirrors one
    bit across present/enabled/health rather than tracking them separately.
    """
    from pymavlink import mavutil

    status = link.mav.recv_match(type="SYS_STATUS", blocking=True, timeout=2.0)
    assert status is not None, "no SYS_STATUS received"

    bit = mavutil.mavlink.MAV_SYS_STATUS_LOGGING
    assert status.onboard_control_sensors_present & bit, "LOGGING not set in present"
    assert status.onboard_control_sensors_enabled & bit, "LOGGING not set in enabled"
    assert status.onboard_control_sensors_health & bit, "LOGGING not set in health"


def test_battery_sensor_bit_is_always_set(link):
    from pymavlink import mavutil

    status = link.mav.recv_match(type="SYS_STATUS", blocking=True, timeout=2.0)
    assert status is not None, "no SYS_STATUS received"

    bit = mavutil.mavlink.MAV_SYS_STATUS_SENSOR_BATTERY
    assert status.onboard_control_sensors_present & bit, "SENSOR_BATTERY not set in present"
    assert status.onboard_control_sensors_enabled & bit, "SENSOR_BATTERY not set in enabled"
    assert status.onboard_control_sensors_health & bit, "SENSOR_BATTERY not set in health"


def test_battery_fields_match_battery_status(link):
    """SYS_STATUS and BATTERY_STATUS read Battery:: independently, on
    different schedule entries (1000 ms vs 2000 ms) -- see design.md's risk
    note. Close, not necessarily bit-identical, is the contract.

    Tolerance is derived, not guessed: lib/Battery/Battery.cpp's remaining()
    maps voltage to percent as (v - 3.5) / (4.2 - 3.5) * 100, a slope of
    100/700 = ~0.143 points per mV. A voltage tolerance below is picked from
    the board's observed ADC jitter between two reads a fraction of a second
    apart; the percent tolerance is that same voltage tolerance carried
    through the identical slope, not an independently chosen number.
    """
    VOLTAGE_TOLERANCE_MV = 100
    PERCENT_TOLERANCE = round(VOLTAGE_TOLERANCE_MV * (100.0 / 700.0))
    deadline = time.time() + 5.0
    sys_status = None
    battery_status = None
    while time.time() < deadline and (sys_status is None or battery_status is None):
        msg = link.mav.recv_match(
            type=["SYS_STATUS", "BATTERY_STATUS"], blocking=True,
            timeout=max(0.1, deadline - time.time()),
        )
        if msg is None:
            continue
        if msg.get_type() == "SYS_STATUS":
            sys_status = msg
        else:
            battery_status = msg

    assert sys_status is not None, "no SYS_STATUS received"
    assert battery_status is not None, "no BATTERY_STATUS received"

    voltage_delta = abs(sys_status.voltage_battery - battery_status.voltages[0])
    assert voltage_delta <= VOLTAGE_TOLERANCE_MV, (
        f"SYS_STATUS voltage_battery={sys_status.voltage_battery} mV vs "
        f"BATTERY_STATUS voltages[0]={battery_status.voltages[0]} mV, "
        f"{voltage_delta} mV apart (tolerance {VOLTAGE_TOLERANCE_MV} mV)"
    )

    remaining_delta = abs(sys_status.battery_remaining - battery_status.battery_remaining)
    assert remaining_delta <= PERCENT_TOLERANCE, (
        f"SYS_STATUS battery_remaining={sys_status.battery_remaining}% vs "
        f"BATTERY_STATUS battery_remaining={battery_status.battery_remaining}%, "
        f"{remaining_delta} points apart (tolerance {PERCENT_TOLERANCE} points)"
    )


def test_errors_comm_counts_parse_errors(link):
    """errors_comm accumulates src/link.cpp's own running total
    (LinkPort::rxDropCount), not the MAVLink library's per-call
    rxstatus.packet_rx_drop_count directly -- that field resets to 0 after
    every single byte, so reading it from SYS_STATUS's independent 1 Hz
    schedule would almost never observe a nonzero value. Forcing a real
    parse error (a valid STX followed by bytes that fail the CRC) is how
    this is told apart from a counter that merely exists but never moves --
    the same distinction that made the write-queue drop counter's own HIL
    case (task 2.3, in the archived change) impossible to force convincingly.
    """
    def read_errors_comm(timeout=2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = link.mav.recv_match(
                type="SYS_STATUS", blocking=True, timeout=max(0.1, deadline - time.time())
            )
            if msg is not None:
                return msg.errors_comm
        return None

    before = read_errors_comm()
    assert before is not None, "no SYS_STATUS received"

    garbage = bytes([0xFD] + [0x55] * 60)
    deadline = time.time() + 2.0
    while time.time() < deadline:
        link.mav.port.write(garbage)
    link.mav.port.flush()

    after = read_errors_comm()
    assert after is not None, "no SYS_STATUS received after sending garbage"
    assert after > before, (
        f"errors_comm did not increase after forced parse errors: "
        f"before={before}, after={after}"
    )
