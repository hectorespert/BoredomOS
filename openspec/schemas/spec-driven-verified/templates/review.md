## Reviewers

<!-- Every agent under .claude/agents/ takes part, on every change. One row each.
     An agent outside its domain answers in a line and stops -- record that, because
     "considered and irrelevant" is information that an absent reviewer does not
     give you. -->

| Agent | Angle | Outcome |
|---|---|---|
| `<name>` | <its domain> | <findings, or "outside its domain because..."> |

## Findings

<!-- Ranked across all reviewers, duplicates merged, most serious first. Judge each
     one rather than relaying it: a confident and wrong reviewer is normal. Where two
     disagreed, say which was right and how you checked. -->

### 1. <what is wrong>

**Where:** `<file>:<line>`
**Why it matters:** <consequence if it shipped>
**Disposition:** fixed in `<file>` / rejected, because... / deferred to `<where>`

## Disagreements

<!-- Where reviewers contradicted each other, what you checked, and the answer.
     Omit if none. -->

## Verdict

<!-- Ready to apply, or not, and what a reader should know before starting --
     particularly anything that cannot be verified with the hardware available. -->
