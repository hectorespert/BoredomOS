# Hardware-in-the-loop checks

These run **on the development machine** and interrogate the satellite over the
MAVLink link. Nothing here is compiled or flashed: the firmware under test is
whatever `pio run -t upload` last put on the board, running exactly as it would in
flight.

That is the point. `test_libs` links `lib/` against the Arduino core and cannot
observe the RTOS at all — no task, no queue, no stack. These checks see only what
the ground station sees, which is the firmware's actual contract. Sixteen cases
across six modules: `check_cli.py` (2) and `check_mode.py` (3) exercise `console-cli`,
`check_clock.py` (1), `check_telemetry.py` (6) and `check_timesync.py` (1) exercise
`mavlink-link`, and `check_recovery.py` (3) exercises `fault-recovery`. Which
scenario each of those covers is not recorded case by case; `TODO.md` carries the
entry for fixing that. `check_recovery.py`'s three are the exception, already named:
`test_first_heartbeat_reports_reduced_state` covers *The ground can see the state
without asking for it → A ground station connects long after a degraded boot*,
`test_heartbeat_reports_operational` covers that requirement's *Normal operation is
not reported as degraded* scenario, and `test_observed_message_set_is_closed` covers
the same scenario's message-set clause.

## Running them

```bash
pip install -r requirements.txt

python run.py                       # every case
python run.py --list                # show the cases without connecting
python run.py --filter heartbeat    # only cases whose name matches
python run.py --port /dev/ttyUSB0   # explicit port (or set HIL_PORT)
```

The port is found automatically: `HIL_PORT` if set, then an Arduino CDC device,
then a lone USB serial adapter. Baud defaults to 57600, matching `LINK_BAUD` in
`include/Link.h`; a CDC port ignores it, so the same default works either way.

**One board port, two things it can carry.** The board's own USB CDC port starts in
CLI mode and switches permanently to MAVLink the first time a complete, valid frame
arrives — that switch is what `run.py` orchestrates: `check_cli.py` and
`check_mode.py`'s cases run first, while the port is still guaranteed to be in CLI
mode, then `run.py` sends the triggering frame and everything else runs against
MAVLink. No adapter is needed for any of this; a bare USB cable reaches both. The
physical radio link on `Serial1` (D0/D1) is untouched and reachable the same way it
always was, with a USB-TTL adapter — point `HIL_PORT` at it to test the radio
specifically instead of the USB endpoint.

## Output

Unity's line format — `file:line:name:PASS|FAIL|IGNORE[: message]` — so `pio test`
would count and colour these natively if the suite were wired in as a
`test_testing_command`. Exit status is 1 if any case failed.

A run where nothing could be checked reports every case as `IGNORE` and exits **0**.
Absent hardware is not a red build; a real failure is.

`check_cli.py` and `check_mode.py`'s cases are gated behind finding the board's own
USB port (`link.is_board_usb`), not behind a working MAVLink link — the whole point
of running first is that no valid frame has arrived yet, so there is nothing MAVLink
to require. Every other case is gated behind `link.sample()` succeeding after
`run.py` sends the triggering frame, same as before.

## Adding a case

Any `test_*` function in a `check_*.py` file, taking the shared `link`. Fail by
raising: `AssertionError` for something wrong, `hil.NoLinkError` for something that
cannot be checked here. `link.sample()` returns one shared 12-second observation, so
the rate checks cost one wait between them rather than one each.

## Under `pio test`

`pio test` runs this suite: it builds `src/`, flashes it, and then runs `run.py`
against the board — no adapter needed, since the board's own USB port carries
everything this suite exercises by default. The Unity suite is opt-in as
`pio test -e libs`, because that one replaces the firmware and erases the card.

Two details make it work, and neither is obvious:

- **`build_stub.cpp`.** PlatformIO counts the sources it compiled from the suite
  directory and bails out before it ever reaches `src/`, so `test_build_src = yes` is
  not enough on its own — a suite that is entirely host-side Python still needs one
  translation unit to exist.
- **`find_port()` waits.** Under `pio test` the checks start seconds after the
  upload, and a USB CDC port takes a moment to re-enumerate after the board resets.
  Without the wait every case is skipped for no real reason.

## One thing not to do

Do not use a 1200-baud touch to reset this board. On the UNO R4 Minima that enters
DFU and stays there — it does not time out into the sketch the way AVR boards do —
and the only way back is to flash.
