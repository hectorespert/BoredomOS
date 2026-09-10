"""Shared helpers for the hardware-in-the-loop checks.

Nothing here talks to the firmware's internals: these checks see exactly what a
ground station sees, which is the whole point of running them against the real
firmware rather than a test binary.
"""

import os
import time
from collections import Counter

DEFAULT_BAUD = 57600  # matches LINK_BAUD in include/Link.h
ARDUINO_VID = 0x2341
PORT_WAIT = 25.0  # seconds; a CDC port re-enumerates slowly after an upload


class NoLinkError(Exception):
    """Raised when no port could be opened. Reported as skipped, not failed."""


def find_port(wait=PORT_WAIT):
    """Return the port to talk to, or raise NoLinkError.

    Order: --port/HIL_PORT, then an Arduino CDC device, then a lone USB serial
    adapter. The adapter case matters because with the default build the link is
    on Serial1 (D0/D1), so the board's own CDC port carries nothing.

    Waits for the port to appear. Under `pio test` this runs seconds after the
    upload, and a USB CDC port takes a moment to re-enumerate after the board
    resets -- without the wait every case would be skipped for no real reason.
    """
    from serial.tools import list_ports

    override = os.environ.get("HIL_PORT")
    if override:
        deadline = time.time() + wait
        while not os.path.exists(override) and time.time() < deadline:
            time.sleep(0.5)
        return override

    deadline = time.time() + wait
    while True:
        candidates = list(list_ports.comports())
        arduino = [p.device for p in candidates if p.vid == ARDUINO_VID]
        if arduino:
            return arduino[0]

        usb = [p.device for p in candidates if "USB" in p.device or "ACM" in p.device]
        if len(usb) == 1:
            return usb[0]
        if len(usb) > 1:
            raise NoLinkError(
                f"several candidate ports ({', '.join(usb)}); set HIL_PORT"
            )
        if time.time() >= deadline:
            raise NoLinkError(f"no serial port found after {wait:.0f}s")
        time.sleep(0.5)


class Link:
    """One connection to the satellite, shared by every check."""

    def __init__(self, port=None, baud=None):
        from pymavlink import mavutil

        self.port = port or find_port()
        self.baud = baud or int(os.environ.get("HIL_BAUD", DEFAULT_BAUD))
        try:
            self.mav = mavutil.mavlink_connection(
                self.port, baud=self.baud, source_system=255, source_component=190
            )
        except Exception as exc:  # pylint: disable=broad-except
            raise NoLinkError(f"cannot open {self.port}: {exc}") from exc
        self._sample = None

    @property
    def is_usb_cdc(self):
        """True when we are talking over the board's own USB CDC port.

        That means the firmware was built with -D LINK_SERIAL=Serial, which changes
        what "the USB port should be silent" is allowed to mean.
        """
        from serial.tools import list_ports

        for p in list_ports.comports():
            if p.device == self.port:
                return p.vid == ARDUINO_VID
        return False

    def sample(self, seconds=12.0):
        """Listen once and cache it, so the rate checks share one observation."""
        if self._sample is not None:
            return self._sample

        counts = Counter()
        identities = set()
        types = set()
        started = time.time()
        while time.time() - started < seconds:
            msg = self.mav.recv_match(blocking=True, timeout=1.0)
            if msg is None:
                continue
            name = msg.get_type()
            counts[name] += 1
            if name == "BAD_DATA":
                continue
            identities.add((msg.get_srcSystem(), msg.get_srcComponent()))
            if name == "HEARTBEAT":
                types.add(msg.type)

        elapsed = time.time() - started
        if not identities:
            raise NoLinkError(
                f"no MAVLink on {self.port} at {self.baud} after {elapsed:.0f}s"
            )
        self._sample = Sample(counts, identities, types, elapsed)
        return self._sample

    def close(self):
        try:
            self.mav.close()
        except Exception:  # pylint: disable=broad-except
            pass


class Sample:
    def __init__(self, counts, identities, types, elapsed):
        self.counts = counts
        self.identities = identities
        self.types = types
        self.elapsed = elapsed

    def rate(self, message):
        return self.counts.get(message, 0) / self.elapsed


def assert_rate(sample, message, expected_hz, tolerance=0.25):
    got = sample.rate(message)
    low, high = expected_hz * (1 - tolerance), expected_hz * (1 + tolerance)
    assert low <= got <= high, (
        f"{message} at {got:.2f} Hz, expected {expected_hz:.2f} Hz "
        f"(seen {sample.counts.get(message, 0)} in {sample.elapsed:.1f}s)"
    )
