#!/usr/bin/env python3
"""Run the hardware-in-the-loop checks against the flashed firmware.

Output is Unity's line format -- file:line:name:PASS|FAIL|IGNORE[: message] --
so `pio test` can count and colour the results natively when this is wired in as
a test_testing_command. Run by hand it is just as readable.

Cases are the test_* functions of every check_*.py in this directory. Each is
given the shared Link, and fails by raising -- an AssertionError for a real
failure, NoLinkError for something that cannot be checked here rather than
something that is wrong.

  python run.py                       every case
  python run.py --filter heartbeat    only cases whose name matches
  python run.py --port /dev/ttyUSB0   explicit port (or set HIL_PORT)
  python run.py --list                show the cases and exit

Exit status is 1 if any case failed. A run where nothing could be checked --
no board, no adapter -- reports every case as IGNORE and exits 0: absent
hardware is not a red build.
"""

import argparse
import importlib
import inspect
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


class ModuleLoadError(Exception):
    """A check module could not be imported, usually a missing dependency."""


def discover():
    """Yield (module_name, function_name, function) in file then definition order."""
    for path in sorted(HERE.glob("check_*.py")):
        try:
            module = importlib.import_module(path.stem)
        except Exception as exc:  # pylint: disable=broad-except
            raise ModuleLoadError(f"{path.name}: {exc}") from exc
        functions = [
            (name, obj)
            for name, obj in vars(module).items()
            if name.startswith("test_") and inspect.isfunction(obj)
            and obj.__module__ == module.__name__
        ]
        functions.sort(key=lambda item: inspect.getsourcelines(item[1])[1])
        for name, func in functions:
            yield path.name, name, func


def report(filename, func, status, message=None):
    line = inspect.getsourcelines(func)[1]
    suffix = f": {message}" if message else ""
    print(f"{filename}:{line}:{func.__name__}:{status}{suffix}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port of the link (or set HIL_PORT)")
    parser.add_argument("--baud", type=int, help="link baud rate (or set HIL_BAUD)")
    parser.add_argument("--filter", help="only run cases whose name contains this")
    parser.add_argument("--list", action="store_true", help="list cases and exit")
    args = parser.parse_args()

    try:
        import hil
    except ImportError as exc:
        print(f"run.py:1:hil_imports:FAIL: {exc}", flush=True)
        print("pymavlink and pyserial are required: pip install pymavlink", flush=True)
        return 1

    try:
        cases = [c for c in discover() if not args.filter or args.filter in c[1]]
    except ModuleLoadError as exc:
        print(f"run.py:1:load_checks:FAIL: {exc}", flush=True)
        print(
            "\nThe checks need pymavlink and pyserial:\n"
            "  pip install -r test/test_hil/requirements.txt",
            flush=True,
        )
        return 1
    if args.list:
        for filename, name, func in cases:
            print(f"{filename}:{inspect.getsourcelines(func)[1]}:{name}")
        return 0
    if not cases:
        print(f"no case matches {args.filter!r}", file=sys.stderr)
        return 1

    if args.port:
        os.environ["HIL_PORT"] = args.port
    if args.baud:
        os.environ["HIL_BAUD"] = str(args.baud)

    link = None
    link_error = None
    try:
        link = hil.Link()
        link.sample()
    except hil.NoLinkError as exc:
        link_error = str(exc)
    except Exception as exc:  # pylint: disable=broad-except
        link_error = f"{type(exc).__name__}: {exc}"

    if link_error:
        for filename, _, func in cases:
            report(filename, func, "IGNORE", link_error)
        print(f"\nNothing was checked: {link_error}", flush=True)
        print(
            "With the default build the link is on Serial1 (D0/D1), so this needs a "
            "USB-TTL adapter or the radio. To use USB instead, flash the bench build:\n"
            '  PLATFORMIO_BUILD_FLAGS="-D LINK_SERIAL=Serial -D LINK_BAUD=115200" '
            "pio run -t upload",
            flush=True,
        )
        return 0

    print(f"link: {link.port} at {link.baud}\n", flush=True)
    failed = 0
    ignored = 0
    for filename, _, func in cases:
        try:
            func(link)
        except AssertionError as exc:
            failed += 1
            report(filename, func, "FAIL", str(exc) or "assertion failed")
        except hil.NoLinkError as exc:
            ignored += 1
            report(filename, func, "IGNORE", str(exc))
        except Exception as exc:  # pylint: disable=broad-except
            failed += 1
            report(filename, func, "FAIL", f"{type(exc).__name__}: {exc}")
        else:
            report(filename, func, "PASS")

    link.close()
    passed = len(cases) - failed - ignored
    print(f"\n{len(cases)} Tests {failed} Failures {ignored} Ignored", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
