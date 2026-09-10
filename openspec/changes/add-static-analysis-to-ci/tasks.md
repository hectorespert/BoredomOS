No step needs the board: this change does not touch firmware. `pio run` and `pio check`
both run without hardware, and the firmware binary is byte-identical before and after.

## 1. Configure the check

- [x] 1.1 Add the `pio check` configuration to `platformio.ini`: `check_tool = cppcheck`,
      `check_src_filters` scoped to `src/`, `include/` and `lib/`,
      `check_skip_packages = yes`, and `check_flags` suppressing `unusedFunction`;
      verify with `pio check` that the summary lists only `src` and `lib` components.
      The filters alone were **not** enough — MAVLink is header-only, so its defects are
      reported inside our translation units. `--suppress=*:*/.pio/libdeps/*` is what
      actually scopes the output: 4823 defects without it, 12 with it.
- [x] 1.2 Confirm the reported total is 12 defects, all LOW, with no HIGH or MEDIUM, and
      that the run takes seconds rather than a minute.
- [x] 1.3 Confirm `pio run` still builds and the firmware is unchanged — compare the
      reported RAM and flash figures against the previous build.

## 1b. Compiler warnings

- [x] 1b.1 Add `-Wall -Wextra` to `build_flags` in `platformio.ini`; verify with a full
      rebuild (`pio run -t clean` then `pio run`) that the warning count across the
      whole build is zero, and that RAM and flash are unchanged at 16828 and 88900
      bytes.

## 2. Wire it into CI

- [x] 2.1 Add a `pio check` step to `.github/workflows/main.yml` after the build step,
      with no `--fail-on-defect`, so defects are reported without failing the job;
      verify the YAML parses. Applied by the repository owner in commit `a0e410e`: the
      push from this session was rejected for lacking GitHub's `workflow` OAuth scope,
      which is required to modify anything under `.github/workflows/`.
- [x] 2.2 Push and confirm on the pull request that the step runs, prints the defect
      table, and reports success.

## 3. Backlog

- [x] 3.1 Delete the `Add static analysis to CI` entry from `TODO.md`, per the workflow
      in `CLAUDE.md`: the change owns this work now. Re-point any cross-reference to it
      first, and verify none dangles.
- [x] 3.2 Record in the change that the entry's premise was wrong — cppcheck reports
      neither the `STATUSTEXT` pointer arithmetic nor the unchecked `pvPortMalloc`
      calls — so that the two backlog defects it named are not assumed to be covered.


## Verification record

`pio check` measured on this tree, three configurations:

| Configuration | Defects printed | Duration |
|---|---|---|
| unconfigured | 4805 | 62 s |
| `check_src_filters` + `check_skip_packages` only | 4823 | 10 s |
| plus `--suppress=*:*/.pio/libdeps/*` | **12** | 10 s |

The middle row is the finding worth keeping: source filters cut the *time* but not the
*output*, because MAVLink is header-only and its defects are reported inside our
translation units. Anyone who tunes this later needs to know that.

Final result: 12 defects, all LOW, no HIGH or MEDIUM — eight `cstyleCast`, three
`unusedLabel` in `src/logger.cpp`, one `constVariable` in `src/mavlink.cpp`, one
`shadowFunction` in `lib/Battery`. All left unfixed on purpose; a first run should show
what the checker actually sees.

`pio run` still produces the same firmware: 88900 bytes of flash, 16828 of RAM,
unchanged from before this change — including after `-Wall -Wextra` were added, which
produce zero warnings across all 104 translation units and do not affect codegen.

Warning flags were chosen on evidence, and the evidence also bounds what they are
worth. Measured on this tree: GCC reports nothing at `-Wall -Wextra -Wpedantic`;
clang's `-Wstring-plus-int`, on by default, does report the `STATUSTEXT` pointer
arithmetic that both GCC and cppcheck miss. Adding a clang pass was considered and
deliberately left out of this change — it is a second toolchain, not a flag.

Two claims in the backlog were checked and found wrong, both now corrected in place:

- The deleted entry claimed cppcheck flags the `STATUSTEXT` pointer arithmetic and the
  unchecked `pvPortMalloc` calls. It flags neither, at any setting tried, including
  `--enable=all --inconclusive` and a custom `<memory><alloc>` library definition
  teaching it that `pvPortMalloc` allocates.
- *Minor leftovers cleanup* claimed static analysis would flag the declaration inside an
  unbraced `case` in `src/mavlink.cpp:175`. It does not. That entry now says so, and
  points at the three `unusedLabel` findings it does produce.

The CI step could not be pushed from this session: modifying anything under
`.github/workflows/` needs the `workflow` OAuth scope, which the token in use does not
have, and commenting the step out does not help — GitHub rejects any modification to
that path, comment or not. The repository owner applied it by hand as `a0e410e`, with
the same content:

```yaml
    - name: Static analysis
      # Reports defects without failing the job: pio check exits 0 unless
      # --fail-on-defect is passed. Tighten to --fail-on-defect=high once the
      # codebase has lived with the checker for a while.
      run: pio check
```

It sits at the end of `.github/workflows/main.yml`, directly after the existing
`Run PlatformIO` step.

**Confirmed on CI** (run 34447638123, commit `a0e410e`): the `Static analysis` step
runs and succeeds, reporting the same 12 defects measured locally — 0 HIGH, 0 MEDIUM,
12 LOW — in 17.9 s on the runner. The job passes, as intended for a warning-only check.
