## What was demonstrated

<!-- Written after apply, from the record. The agents that fill this in read that
     record; they do not produce it. They do not run the suites and do not touch the
     board -- say so in the closing section, every time. -->

### Tasks

| State | Count |
|---|---|
| ticked | |
| open, board unreachable | |
| open, other reason | |

<!-- Every open task listed below with its reason. An open `[board]` step is an
     honest outcome; an unrecorded one is the defect this document exists to catch. -->

### Receipts

<!-- One row per ticked task. A tick is a claim; this column is the proof. "the build
     went green" is only a receipt for something a build can actually show.
     Off the board the HIL suite reports every case as IGNORE and exits 0 -- a receipt
     from it has to show the cases firing, not just a zero exit. -->

| Task | Claim | Receipt | Located |
|---|---|---|---|
| X.Y | <what it says was done> | <command + output, HIL line, SD log entry, build figure> | yes / NO |

### Scenario coverage

<!-- From the delta, not from the task list. A scenario whose only task is unticked is
     unverified -- write that word. A count of ticks is not coverage. -->

| Requirement → Scenario | Task | Method | State |
|---|---|---|---|
| <requirement> → <scenario> | X.Y | T / A / I / R | demonstrated / unverified, needs board / unverified |

### Review dispositions

<!-- Everything review.md recorded as fixed, confirmed fixed. Everything deferred,
     confirmed where it went. Nothing else closes this loop. -->

| Finding | Recorded as | Actually |
|---|---|---|

### Figures

<!-- Any number the change asserted, re-read from the tree. Not quoted from the
     artifacts that asserted it. A figure that contradicts the tree or contradicts
     itself makes the decision PASS_WITH_WARNINGS, not FAIL. Say whether it was wrong
     when written or has since gone stale -- a reader cannot tell those apart later. -->

## Participants

<!-- Not the whole roster. The qa engineer always; any role whose finding was recorded
     as fixed; the firmware engineer if a high-water mark moved. -->

| Agent | Why it took part | What it found |
|---|---|---|

## Decision

<!-- CANONICAL LINE -- keep it exactly, on its own line. Enforcement tooling reads it.
     Replace <VALUE> with exactly one of:
     PASS | PASS_WITH_WARNINGS | PASS_PENDING_BOARD | FAIL

     PASS                 everything promised was demonstrated, record cuts clean
     PASS_WITH_WARNINGS   demonstrated, but a figure in the record needs fixing before
                          archiving
     PASS_PENDING_BOARD   everything reachable without the board was demonstrated, and
                          what remains is listed below with the step that would close
                          each item. The ordinary outcome here, not a failure.
     FAIL                 the delivered change does not do what it promised, or a tick
                          has no receipt. A wrong figure is a warning, not this. -->

DECISION: <VALUE>

### Still to demonstrate

<!-- Each item with the step that would close it. Required for PASS_PENDING_BOARD and
     FAIL; omit when nothing remains. -->

### Limit of this audit

<!-- State every time: the record was read, the suites were not run and the board was
     not touched. -->
