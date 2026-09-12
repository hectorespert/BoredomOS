## Test plan

<!-- Written by the qa engineer, before the task list exists. One row per
     `#### Scenario:` in the change's delta specs -- every scenario gets one. A scenario
     missing here is a defect in this document, not something tasks.md can paper over.

     The mapping is a floor. Extra cases and regression checks are welcome and need no
     row; they never substitute for one.

     Methods: T test · A analysis · I inspection · R review of design · D demonstration
     (a person at the board doing a physical act -- the bulk of this project). -->

| Row | Requirement → Scenario | Method | Where it runs | Receipt | Board |
|---|---|---|---|---|---|
| TP-1 | <requirement> → <scenario> | T | `test/test_hil/check_<x>.py` | the `file:line:name:PASS` line from a run that fired | yes |
| TP-2 | <requirement> → <scenario> | A | `scripts/<check>.py` at build time | what the check printed | no |
| TP-3 | <requirement> → <scenario> | D | pull the SD card while running | the phase marker the next boot reports | yes, and hands |

## Rows nothing can reach

<!-- A scenario no method available here can demonstrate. Say so now, while the
     requirement can still be reworded -- not after it is contract. Do not invent a
     method that cannot run to make a row look closed. Omit if none. -->

## Notes

<!-- Shared fixtures, an adapter that has to be wired, an order the acts have to happen
     in, anything that makes a receipt harder to get than the row suggests.

     If the roster holds no qa engineer, say that here rather than choosing the methods
     in its place and calling this a plan. -->
