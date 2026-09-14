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

# Cases in these files exercise the console CLI and must run before anything
# sends a valid MAVLink frame on the same port -- once that happens the
# switch to MAVLink mode is permanent for the rest of the boot
# (specs/console-cli/spec.md) and the CLI becomes unreachable. run.py uses
# this to order cases into a CLI phase and a MAVLink phase, calling
# Link.enter_mavlink_mode() only between them.
CLI_PHASE_FILES = frozenset({"check_cli.py", "check_mode.py"})


class NoLinkError(Exception):
    """Raised when no port could be opened. Reported as skipped, not failed."""


def find_port(wait=PORT_WAIT):
    """Return the port to talk to, or raise NoLinkError.

    Order: --port/HIL_PORT, then an Arduino CDC device, then a lone USB serial
    adapter. Since add-usb-dual-protocol there is one board port for
    everything: the Arduino CDC device is the USB endpoint, which starts in
    CLI mode and switches permanently to MAVLink on the first valid frame
    (see Link.enter_mavlink_mode()) -- it is preferred over a lone USB-serial
    adapter, which would mean a USB-TTL adapter wired to the radio UART on
    D0/D1 instead. Both carry MAVLink; only the port differs.

    Waits for the port to appear. Under `pio test` this runs seconds after the
    upload, and a USB CDC port takes a moment to re-enumerate after the board
    resets -- without the wait every case would be skipped for no real reason.
    """
    from serial.tools import list_ports

    override = os.environ.get("HIL_PORT")
    if override:
        # Only a device path can be waited for. pymavlink also accepts endpoints
        # like udp:127.0.0.1:14550, which never exist on the filesystem.
        if not override.startswith("/") and not override.upper().startswith("COM"):
            return override
        deadline = time.time() + wait
        while not os.path.exists(override) and time.time() < deadline:
            time.sleep(0.5)
        if not os.path.exists(override):
            raise NoLinkError(f"{override} did not appear after {wait:.0f}s")
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
        self._mavlink_mode = False

    @property
    def is_board_usb(self):
        """True when self.port is the board's own USB CDC device.

        False when HIL_PORT points elsewhere -- a USB-TTL adapter or the
        radio wired to D0/D1, testing the physical radio link rather than the
        board's own USB endpoint. The console CLI only ever lives on the
        board's own port, so CLI-phase cases use this to self-skip when it is
        not the one in use.
        """
        from serial.tools import list_ports

        for p in list_ports.comports():
            if p.device == self.port:
                return p.vid == ARDUINO_VID
        return False

    def enter_mavlink_mode(self):
        """Send one HEARTBEAT to switch the port into MAVLink mode.

        Only meaningful on the board's own USB CDC port, where the switch is
        one-way and permanent for the boot (specs/console-cli/spec.md, "The
        USB port chooses its protocol from what arrives on it"). Opening a
        pymavlink connection sends nothing by itself -- CLI-phase cases in
        run.py rely on exactly that to run before this is ever called. Safe
        to call more than once: the second HEARTBEAT is just ordinary
        traffic once already switched. Idempotent per Link instance so
        callers do not have to track whether it was already sent.
        """
        if self._mavlink_mode:
            return
        from pymavlink import mavutil

        self.mav.mav.heartbeat_send(
            mavutil.mavlink.MAV_TYPE_GCS, mavutil.mavlink.MAV_AUTOPILOT_INVALID, 0, 0, 0
        )
        self._mavlink_mode = True

    def sample(self, seconds=12.0):
        """Listen once and cache it, so the rate checks share one observation."""
        if self._sample is not None:
            return self._sample

        self.enter_mavlink_mode()

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
