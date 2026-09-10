"""TIMESYNC is answered.

Covers the mavlink-link scenario about TIMESYNC. The sub-second part of the reply
is expected to be zero: SystemTime has one-second resolution, which is a known
limitation tracked in TODO.md, not a defect of the link.
"""

import time


def test_timesync_is_answered(link):
    ts1 = int(time.time() * 1e9)
    link.mav.mav.timesync_send(0, ts1)

    reply = None
    started = time.time()
    while time.time() - started < 5.0:
        msg = link.mav.recv_match(type="TIMESYNC", blocking=True, timeout=1.0)
        if msg and msg.tc1 != 0:
            reply = msg
            break

    assert reply is not None, "TIMESYNC with tc1 == 0 was not answered"
    assert reply.ts1 == ts1, f"ts1 was not echoed back: sent {ts1}, got {reply.ts1}"
    assert reply.tc1 > 0, "reply carries no timestamp"
