"""The console CLI answers text commands on the USB console.

Covers the console-cli capability's core round trip: `ps` lists every task the
kernel reports, ending with free heap, and `free` reports the heap's three
figures.

Runs in run.py's CLI phase, before anything sends a valid MAVLink frame:
since add-usb-dual-protocol the port starts in CLI mode and switches
permanently to MAVLink on the first one (specs/console-cli/spec.md), so these
cases share `link`'s own connection (link.mav.port, the raw serial object
pymavlink itself reads from) rather than opening a second one -- there is only
one port now, and nothing has sent anything on it yet at this point in the run.

Skipped when `link` is not the board's own USB CDC device -- HIL_PORT pointed
at a USB-TTL adapter or the radio on D0/D1 tests the physical radio link
instead, which never carries the CLI.
"""

import time

from hil import NoLinkError


def _send_command(console, command):
    console.reset_input_buffer()
    console.write((command + "\n").encode("ascii"))
    console.flush()
    time.sleep(0.5)
    return console.read(console.in_waiting or 1).decode("ascii", errors="replace")


def test_cli_ps_lists_every_task(link):
    if not link.is_board_usb:
        raise NoLinkError("link is not the board's own USB port, so the CLI is not on it")

    console = link.mav.port
    reply = _send_command(console, "ps")

    lines = [line for line in reply.splitlines() if line.strip()]
    assert lines, f"no reply to 'ps': {reply!r}"
    assert lines[0].startswith("ID"), f"unexpected ps header: {lines[0]!r}"

    # Every reply ends with the "> " prompt (src/cli.cpp's printPrompt()), which
    # is not a task row -- drop it explicitly rather than by content-guessing.
    body = lines[1:]
    if body and body[-1].strip() == ">":
        body = body[:-1]
    rows = [line for line in body if not line.startswith("heap free")]

    # The reduced configuration starts only 5 firmware tasks (including Cli
    # itself) plus IDLE -- 6 rows is the floor in any configuration, not the
    # 9 the normal configuration shows.
    assert len(rows) >= 6, f"expected at least 6 task rows, got {len(rows)}: {rows}"

    for row in rows:
        fields = row.split()
        assert len(fields) >= 5, f"row does not parse: {row!r}"

    assert any(line.startswith("heap free:") for line in lines), (
        f"'ps' reply has no free-heap line: {reply!r}"
    )


def test_cli_free_reports_three_figures(link):
    if not link.is_board_usb:
        raise NoLinkError("link is not the board's own USB port, so the CLI is not on it")

    console = link.mav.port
    reply = _send_command(console, "free")

    lines = [line for line in reply.splitlines() if line.strip()]
    for label in ("heap total:", "heap free:", "heap free min:"):
        assert any(line.startswith(label) for line in lines), (
            f"'free' reply missing {label!r}: {reply!r}"
        )
