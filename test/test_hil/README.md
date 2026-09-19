# Hardware-in-the-loop checks

These run **on the development machine** and interrogate the satellite over the
MAVLink link. Nothing here is compiled or flashed: the firmware under test is
whatever `pio run -t upload` last put on the board, running exactly as it would in
flight.

That is the point. `test_libs` links `lib/` against the Arduino core and cannot
observe the RTOS at all — no task, no queue, no stack. These checks see only what
the ground station sees, which is the firmware's actual contract. Nine of the twelve
cases exercise the `mavlink-link` capability, but which scenario each of those covers
is not recorded: nine cases against eight scenarios, and six of the cases name none.
`TODO.md` carries the entry for fixing that.

The other three, in `check_recovery.py`, exercise `fault-recovery`:
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

**A link has to be reachable from this machine, and since
replace-console-cli-with-usb-mavlink-link there are two.** The flight build
carries MAVLink on the D0/D1 UART *and* on USB CDC, in every build, so the
board's own cable is enough and no adapter is needed for most of the suite.
There is no `bench` environment any more and nothing to reflash.

Point `HIL_PORT` at a **device path** to target the UART instead — an adapter or
the radio on D0/D1:

```bash
HIL_PORT=/dev/ttyUSB0 python run.py
```

`check_dual_link.py` is the exception: its cases need *both* ports at once, so
they read the board's CDC port themselves and take `HIL_UART_PORT` for the
other. Without an adapter they self-skip, and a green run has not exercised
them.

## Output

Unity's line format — `file:line:name:PASS|FAIL|IGNORE[: message]` — so `pio test`
would count and colour these natively if the suite were wired in as a
`test_testing_command`. Exit status is 1 if any case failed.

A run where nothing could be checked reports every case as `IGNORE` and exits **0**.
Absent hardware is not a red build; a real failure is.

Every case is gated behind a working link. `check_usb_link.py` strictly speaking
only needs the USB port, and it is still gated: with no link reachable, "USB
carries MAVLink" cannot tell a working board from a dead one, and a green result
there would be a false pass.

It replaced `check_silence.py`, which asserted the opposite — that USB carried no
frames — and was correct until USB became a link.

## Adding a case

Any `test_*` function in a `check_*.py` file, taking the shared `link`. Fail by
raising: `AssertionError` for something wrong, `hil.NoLinkError` for something that
cannot be checked here. `link.sample()` returns one shared 12-second observation, so
the rate checks cost one wait between them rather than one each.

## Under `pio test`

`pio test` runs this suite: it builds `src/`, flashes it, and then runs `run.py`
against the board, over USB, with no adapter attached. The Unity suite is opt-in
as `pio test -e libs`, because that one replaces the firmware and erases the
card.

Two details make it work, and neither is obvious:

- **`build_stub.cpp`.** PlatformIO counts the sources it compiled from the suite
  directory and bails out before it ever reaches `src/`, so `test_build_src = yes` is
  not enough on its own — a suite that is entirely host-side Python still needs one
  translation unit to exist.
- **`find_port()` waits.** Under `pio test` the checks start seconds after the
  upload, and a USB CDC port takes a moment to re-enumerate after the board resets.
  Without the wait every case is skipped for no real reason.

## Steps no script here can run

Two things this suite cannot observe, recorded so they are not mistaken for
covered. Both belong to `replace-console-cli-with-usb-mavlink-link`.

- **A host that opens USB and stops reading must not reset the board.**
  `_SerialUSB::write()` loops without yielding when its buffer is full, so a
  stalled host above idle priority would keep `vApplicationIdleHook()` from
  refreshing the watchdog. `TaskLinkWrite` guards this with
  `availableForWrite()`. To check it: open `/dev/ttyACM0`, read nothing, and
  watch the UART link with MAVProxy — telemetry must keep its cadence, the SD
  log must keep writing, and the board must not reset. Automating it means
  holding a port open and *not* draining it for long enough to matter, which is
  the opposite of what every helper here does.
- **The 8-value housekeeping cycle in the normal configuration**, which
  `add-mavlink-housekeeping-telemetry` also left open and which is now a 9-value
  cycle. A board in the reduced configuration exercises a shorter set, so a pass
  there proves less than it appears to.

## One thing not to do

Do not use a 1200-baud touch to reset this board. On the UNO R4 Minima that enters
DFU and stays there — it does not time out into the sketch the way AVR boards do —
and the only way back is to flash.
