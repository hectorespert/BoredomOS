"""The console CLI answers text commands on the USB console.

Covers the console-cli capability's core round trip: `ps` lists every task the
kernel reports, ending with free heap, and `free` reports the heap's three
figures. This is the CLI's mirror image of check_silence.py's proof that the
port carries nothing when nobody speaks to it -- see design.md, "Resolved while
writing the task list".

Skipped when the link itself is on USB (-D LINK_SERIAL=Serial, the bench
build): CLI_SERIAL has then moved to Serial1, per include/Cli.h, and there is
no adapter here to reach it.
"""

import time

from hil import ARDUINO_VID, NoLinkError


def _usb_cdc_port():
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == ARDUINO_VID:
            return p.device
    return None


def _open_console():
    if _open_console.port is None:
        _open_console.port = _usb_cdc_port()
    if _open_console.port is None:
        raise NoLinkError("the board's USB CDC port is not attached")

    import serial

    console = serial.Serial(_open_console.port, 115200, timeout=2)
    console.dtr = True
    time.sleep(0.5)
    console.reset_input_buffer()
    return console


_open_console.port = None


def _send_command(console, command):
    console.write((command + "\n").encode("ascii"))
    console.flush()
    time.sleep(0.5)
    return console.read(console.in_waiting or 1).decode("ascii", errors="replace")


def test_cli_ps_lists_every_task(link):
    if link.is_usb_cdc:
        raise NoLinkError("the link is on USB in this build, so the CLI is on Serial1")

    with _open_console() as console:
        reply = _send_command(console, "ps")

    lines = [line for line in reply.splitlines() if line.strip()]
    assert lines, f"no reply to 'ps': {reply!r}"
    assert lines[0].startswith("ID"), f"unexpected ps header: {lines[0]!r}"

    rows = [line for line in lines[1:] if not line.startswith("heap free")]
    # At least the seven tasks the firmware creates in the normal
    # configuration, plus this change's own CLI task -- see tasks.md task 5.2.
    assert len(rows) >= 7, f"expected at least 7 task rows, got {len(rows)}: {rows}"

    for row in rows:
        fields = row.split()
        assert len(fields) >= 5, f"row does not parse: {row!r}"

    assert any(line.startswith("heap free:") for line in lines), (
        f"'ps' reply has no free-heap line: {reply!r}"
    )


def test_cli_free_reports_three_figures(link):
    if link.is_usb_cdc:
        raise NoLinkError("the link is on USB in this build, so the CLI is on Serial1")

    with _open_console() as console:
        reply = _send_command(console, "free")

    lines = [line for line in reply.splitlines() if line.strip()]
    for label in ("heap total:", "heap free:", "heap free min:"):
        assert any(line.startswith(label) for line in lines), (
            f"'free' reply missing {label!r}: {reply!r}"
        )
