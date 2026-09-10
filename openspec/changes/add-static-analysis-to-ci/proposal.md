## Why

CI runs `pio run` and nothing else: it proves the firmware compiles, not that it is
sound. PlatformIO ships cppcheck behind `pio check`, so adding static analysis costs a
step in the existing job.

Measuring it before configuring it changed what this change should be. Run as-is,
`pio check` reports **4805 defects**, of which **4786 come from `.pio/libdeps/`** —
4584 from MAVLink's generated headers alone. Unscoped, the signal from this project's
own code is 0.4% of the output, which is precisely how a check ends up disabled.

## What Changes

- `platformio.ini` gains a `pio check` configuration: cppcheck **and clang-tidy**, scoped to `src/`,
  `include/` and `lib/`, skipping installed packages, with `unusedFunction` and
  everything under `.pio/libdeps/` suppressed. The path suppression is the load-bearing
  part — MAVLink is header-only, so its defects surface inside our translation units and
  source filters alone do not silence them (4823 defects with filters only, 12 with the
  suppression).
- `platformio.ini` gains `-Wall -Wextra` to `build_flags`. The tree already builds
  clean under them: measured 0 warnings across the whole build, dependencies included,
  and the firmware is byte-identical (16828 bytes of RAM, 88900 of flash, before and
  after). They cost nothing today and catch the next regression rather than the next
  reader.
- `.github/workflows/main.yml` gains a `pio check` step that runs after the build and
  **does not fail the job**. `pio check` exits 0 unless `--fail-on-defect` is passed,
  so warning-only is the default behaviour rather than something to arrange.

`unusedFunction` is suppressed because every one of its hits here is wrong: cppcheck
cannot see that task bodies are reached through `xTaskCreate` function pointers, that
`setup()` and `loop()` are called by the Arduino core, or that `lib/` functions are
used from another translation unit.

With that configuration cppcheck reports **12 defects, all LOW**: eight C-style casts,
three `unusedLabel` in `src/logger.cpp`, and one `constVariable`.

clang-tidy is enabled alongside it because the two tools do not overlap: it finds
**5 MEDIUM defects cppcheck does not see at all**, two of which are real. Its default
check set had to be replaced — see `design.md`, Decision 6.

## Correcting the backlog entry that asked for this

The `TODO.md` entry justified static analysis by claiming *"the pointer arithmetic of
[the unknown-message `STATUSTEXT`] and the unchecked `pvPortMalloc` calls are exactly
the kind of defect cppcheck flags"*. **That is not true**, and it was checked rather
than assumed:

- `src/mavlink.cpp:208` — `sendStatusText("literal " + msg->msgid, ...)` is not
  reported at any setting, including `--enable=all --inconclusive`. Adding an integer
  to a `const char*` is valid C++; nothing marks it as a mistake.
- The unchecked `pvPortMalloc` dereferences are not reported either, because cppcheck
  does not know `pvPortMalloc` allocates. Teaching it with a `<memory><alloc>` library
  definition was tried and still produced nothing on cppcheck 2.11, the version
  PlatformIO ships.

So this change buys a style and portability net, not a bug net. It is still worth
having — but the entry's premise does not survive contact, and the defects it named
still need fixing by hand.

## Capabilities

None. This is tooling: it changes no firmware behaviour, so `.openspec.yaml` sets
`skip_specs: true`.

## Impact

**RAM.** None. No task, queue or library is added; the firmware binary is untouched.

**MAVLink surface.** Unchanged.

**Code.** `platformio.ini` and `.github/workflows/main.yml` only. No `src/`, `include/`
or `lib/` file is modified, and the 12 reported defects are left alone deliberately —
fixing them is separate work, and doing it here would hide what the check actually
found on its first run.

**CI.** One extra step, about 11 s scoped (61 s unscoped). It cannot fail the build.
