"""Mode detection on the USB console: stays in CLI mode until a valid MAVLink
frame arrives, then switches for the rest of the boot.

Covers specs/console-cli/spec.md's "The USB port chooses its protocol from
what arrives on it": a lone header byte or line noise must not switch it, and
a complete, checksum-valid frame must.

Runs in run.py's CLI phase (hil.CLI_PHASE_FILES), after check_cli.py -- both
files' cases run with the port still in CLI mode, and this file's own cases
are ordered so the one that switches the mode runs last (source-line order is
what run.py sorts by), so it does not disturb any other CLI-phase case.
"""

import os
import time

from hil import NoLinkError


def _send(console, data):
    console.reset_input_buffer()
    console.write(data)
    console.flush()
    time.sleep(0.5)
    return console.read(console.in_waiting or 1)


def test_mode_stays_cli_under_lone_header_byte(link):
    if not link.is_board_usb:
        raise NoLinkError("link is not the board's own USB port")

    console = link.mav.port
    # The header byte lands in the CLI's own line buffer (it never completes a
    # frame), so it corrupts *that* line the same way any stray byte would --
    # design.md's "Line noise on an open port" scenario, not a mode switch.
    # Close it with its own newline before checking that the CLI still works.
    _send(console, bytes([0xFD]) + b"\n")
    reply = _send(console, b"free\n")
    assert b"heap total:" in reply, (
        f"CLI did not answer 'free' after a lone 0xFD -- mode may have switched: {reply!r}"
    )


def test_mode_stays_cli_under_line_noise(link):
    if not link.is_board_usb:
        raise NoLinkError("link is not the board's own USB port")

    console = link.mav.port
    _send(console, os.urandom(20) + b"\n")
    reply = _send(console, b"free\n")
    assert b"heap total:" in reply, (
        f"CLI did not answer 'free' after line noise -- mode may have switched: {reply!r}"
    )


def test_mode_switches_after_valid_frame(link):
    """The one case in this suite that triggers the switch. Must stay last in
    this file (and this file must stay the last CLI-phase one, per
    hil.CLI_PHASE_FILES's alphabetical position after check_cli.py) -- nothing
    else may assume CLI mode after this runs.
    """
    if not link.is_board_usb:
        raise NoLinkError("link is not the board's own USB port")

    console = link.mav.port
    console.reset_input_buffer()
    link.enter_mavlink_mode()  # the one trigger the whole suite relies on

    hb = link.mav.recv_match(type="HEARTBEAT", blocking=True, timeout=5)
    assert hb is not None, "no HEARTBEAT seen after entering MAVLink mode"

    reply = _send(console, b"free\n")
    assert b"heap total:" not in reply, (
        f"CLI still answered 'free' after a valid frame -- mode did not switch: {reply!r}"
    )
