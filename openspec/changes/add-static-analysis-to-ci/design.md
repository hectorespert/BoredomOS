## Context

See `proposal.md` — Why, and the correction to the backlog entry. What follows is the
measurement the configuration is derived from.

`pio check` unscoped, on this tree:

| Component | HIGH | MEDIUM | LOW |
|---|---|---|---|
| `.pio/libdeps/MAVLink/mavlink/common` | 0 | 0 | 4584 |
| `.pio/libdeps/MAVLink/mavlink` (rest) | 0 | 6 | 170 |
| other `.pio/libdeps/*` | 1 | 1 | 24 |
| `src`, `lib` | 0 | 0 | 24 |

The dependencies are not ours to fix and MAVLink's headers are generated, so the only
rows worth reading are the last one.

## Goals / Non-Goals

**Goals:**

- The check reports on this project's code and nothing else.
- The first version cannot fail the build, so it cannot be the reason CI is disabled.
- The configuration lives in `platformio.ini`, so `pio check` locally and `pio check`
  in CI report the same thing.

**Non-Goals:**

- Fixing any of the 12 reported defects. That is separate work; leaving them visible is
  the point of a first run.
- Adding a second analyser (clang-tidy, PVS-Studio). One tool, understood, first.
- Failing the build on anything, yet.

## Decisions

### 1. Scoping needs a cppcheck path suppression, not just `check_src_filters`

`check_src_filters` and `check_skip_packages` choose which *source files* are analysed.
They are necessary but not sufficient here, and the reason is worth writing down because
it is not obvious: **MAVLink is header-only**. Its 4584 defects are not in files
PlatformIO would hand to the checker as sources — they are in headers that
`#include` pulls into `src/serial.cpp` and `src/mavlink.cpp`. cppcheck reports a defect
where it finds it, so scoping the inputs leaves the output unchanged.

What actually works is suppressing by path, in `check_flags`:

```
cppcheck: --suppress=unusedFunction --suppress=*:*/.pio/libdeps/*
```

Measured: 4823 defects reported with the filters alone, 12 with the suppression added.
The filters stay because they cut the run from 61 s to 10 s by not analysing dependency
sources; the suppression is what silences dependency *headers*.

`include/` is in the filter even though it holds only headers: `Data.h` defines the log
schema and `Link.h` the port aliases, and both are worth checking.

### 2. Suppress `unusedFunction`, nothing else

Every hit is a false positive caused by cppcheck's single-translation-unit view:

| Reported unused | Actually called by |
|---|---|
| `TaskSerialRead`, `TaskSerialWrite`, `TaskHeartbeat`, `TaskMavlinkBatteryStatus`, `TaskMavlink`, `TaskLogger`, `TaskSdWrite` | `xTaskCreate`, through a function pointer |
| `setup`, `loop` | the Arduino core's `main()` |
| `getUnixTimeUsec`, `getUnixTimeNsec` | `src/mavlink.cpp`, a different translation unit |

Suppressing the check is better than annotating ten call sites, and it cannot hide a
real finding: a genuinely dead function in this codebase would be caught in review, not
by a checker that thinks every task body is dead.

The other three categories stay visible on purpose. `cstyleCast` (8) is a real if
stylistic observation about `(mavlink_message_t*)pvPortMalloc(...)`. `constVariable`
(1) is correct. And `unusedLabel` (3) is the most interesting of the lot — see below.

### 3. Warning-only comes free

`pio check` exits 0 unless `--fail-on-defect` is passed. The step is therefore plain
`pio check` with no flag and no `continue-on-error:`. Tightening later means adding
`--fail-on-defect=high`, which today would pass with zero HIGH defects — but committing
to that before the code has lived with the checker for a while is what the backlog entry
warned against.

### 4. The `unusedLabel` findings are not noise

`src/logger.cpp:26,29,31` are reported as unused labels. They are not labels — they are
GCC's legacy designated-initializer syntax:

```cpp
Data data = {
    unixtime: systemTime.getUnixTime(),
    uptime:   xTaskGetTickCount() * portTICK_PERIOD_MS,
    ...
};
```

cppcheck parses `unixtime:` as a goto label because that is what it is in standard C++.
The construct is a GNU extension that GCC still accepts. It matters beyond style: this
is the same initialiser whose omitted `energy` member is silently zeroed, which is the
backlog defect *The SD log never stores the battery data*. A checker pointing at that
line on its first run is the check earning its place.

Left unfixed here, deliberately — changing that initialiser is firmware work and belongs
with the `energy` defect, not with a CI change.

### 5. Compiler warnings are enabled because they are free, not because they help here

`-Wall -Wextra` were added after measuring, not before. A full rebuild of all 104
translation units produces **zero warnings** — not zero in `src/`, zero anywhere,
dependencies included — and the firmware is byte-identical with and without them.

Being honest about what that buys: nothing today. GCC at any warning level tried,
including `-Wpedantic`, reports neither of the two defects the backlog cares about.
What it buys is that the counter is at zero, so the first warning that ever appears is
signal rather than something to scroll past. That property is worth having and is only
cheap to establish while the count is already zero.

It also makes `-Werror` a defensible future step, which it would not be against a
codebase carrying a backlog of warnings.

### 6. clang-tidy earns a place, but only with its default checks thrown away

Run with PlatformIO's defaults, clang-tidy reports **248 MEDIUM defects** against
cppcheck's 12. Practically none are about this firmware: they are LLVM house style —
`modernize-use-trailing-return-type`, `readability-magic-numbers`,
`hicpp-uppercase-literal-suffix`, `llvm-header-guard`, and
`llvmlibc-implementation-in-namespace`, which asks that `Battery::voltage` be declared
inside `__llvm_libc`.

Restricted to the families that look for defects rather than for style —
`bugprone-*`, `cert-*`, `clang-analyzer-*`, minus `cert-err58-cpp`, which warns about
static constructors throwing on a target with no exceptions — it reports 5, and is
three times faster (7 s against 24 s).

Those 5, and what each is worth:

| Finding | Verdict |
|---|---|
| `lib/SdData/SdData.cpp:6` — two adjacent `int` parameters easily swapped | **Real.** `SdData(int files, size_t size)` is exactly the confusion behind `TEST_FILE_SIZE_MB = 1024UL` being bytes, which the backlog already tracks |
| `src/mavlink.cpp:177` — narrowing `unsigned long long` to `time_t` | **Real.** This is the timestamp arriving from the ground; it belongs with the backlog entry about validating what the GCS sends |
| `src/main.cpp:62` — suspicious `sizeof(A*)` | Deliberate: it is the queue-carries-pointers protocol |
| `src/mavlink.cpp:154,170` — consecutive identical switch branches | Deliberate: the reserved empty cases documented in ARCHITECTURE.md |

The three deliberate ones are left visible rather than suppressed. A checker pointing at
the two most easily misread constructs in the firmware is doing its job, and a reader who
has to look up why `sizeof(Data*)` is intentional will find the answer in
`ARCHITECTURE.md` §4.

What clang-analyzer did **not** find is worth recording too: no null-dereference on the
unchecked `pvPortMalloc` calls. Path-sensitive analysis does not help when the tool does
not know the function allocates.

## Risks / Trade-offs

- **A warning-only check is a check nobody reads** → accepted for now, because the
  alternative failed before it started: an unscoped, build-failing check would have
  reported 4805 defects on its first run. The path to enforcement is
  `--fail-on-defect=high`, and the defect counts in `proposal.md` are the baseline to
  measure the next tightening against.
- **`unusedFunction` suppression could hide real dead code** → mitigated by the table
  above: the suppression exists for a structural reason (function pointers and
  cross-TU calls), not to quiet an inconvenient result.
- **cppcheck's version is whatever PlatformIO ships** (2.11 today), so results can
  shift under a toolchain update → this is exactly why the step cannot fail the build.
  The same applies to clang-tidy, and more sharply: check names come and go between
  releases, and a `--checks=` list can silently stop matching.
- **clang-tidy adds ~7 s locally and a one-time toolchain download on a cold CI cache**
  → acceptable against a build that already takes longer, and the PlatformIO cache step
  already in the workflow covers the repeat cost.
- **The check runs on the same job as the build**, so a cppcheck crash would show as a
  CI failure even though it cannot fail on defects → acceptable; a crashing analyser is
  worth knowing about.
