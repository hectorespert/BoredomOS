"""The clock can be set from the ground.

Covers the mavlink-link scenario "inbound SYSTEM_TIME and TIMESYNC sent on that
port are acted upon". Note this WRITES to the satellite's DS1307.
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


def test_inbound_system_time_sets_the_clock(link):
    host = int(time.time())
    link.mav.mav.system_time_send(host * 1_000_000, 0)
    time.sleep(2.5)
    after = _read_unix_time(link)
    drift = abs(after - int(time.time()))
    assert drift <= 2, f"clock is {drift}s off after being set from the ground"
