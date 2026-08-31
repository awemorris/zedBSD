# Queue: producer-linked public reference reconciliation

Last updated: 2026-08-31

QID: `q046`

Queue status: completed

Queue finished: **Yes**

Authorization: the user directed continuous Queue creation and execution after
the current corrections, deferring human-judgment items while continuing every
independent ready workstream. A full dependency audit found no remaining code
Phase that can currently finish without a human, physical, Noct, or fresh-image
gate. These two WS009 Phases describe already implemented producer contracts
and require none of those blocked inputs.

Timebox: none. Reconcile both public references with production source and
retained tests, validate links/source anchors, synchronize P/W/M, commit
locally, and immediately audit the next Queue.

Parent: [master plan](master.md)

Previous Queue: [q045](queue-q045.md)

## Purpose

Remove stale public evdev/input claims after q044 and formally close the
already substantial common kernel boot-parameter reference against the q015,
q031, and q032 implementation boundary.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws009-p004` | [Phase](ws009-documentation/phase004-evdev-input-reference/phase.md) | completed | The evdev reference states current per-source, character-only, overflow/resync, console-subscriber, and detach behavior without claiming live USB HID |
| 2 | `ws009-p005` | [Phase](ws009-documentation/phase005-kernel-parameter-reference/phase.md) | completed | Every common boot parameter, default, rejection rule, and four-x86-loader path matches production source and retained evidence |

## Fixed boundaries

- Product documentation only; no source, UAPI, loader, Noct, or behavior
  change is authorized.
- Current, experimental, deprecated, and planned behavior remain distinct.
- Every corrected claim points to a public header, production owner, or
  executable producer test.
- Use WS009 DOC-T00/T10/T31/T50 checks. Do not run aggregate `make check` or
  consume `.internal/` material.

## Completion definition

q046 finishes when both references are reconciled, their Phase/WS/master status
is synchronized, all relative links and declared source anchors resolve, and
each item is marked `completed` or `uncleared`. A semantic disagreement between
approved contract and production code is returned to the producer WS and does
not authorize a source change in this Queue.

## Result

Both producer-linked references were reconciled without changing production
source or UAPI. The evdev reference now distinguishes physical and momentary
sources, per-device state, reader-local and HAL-source resynchronization,
console subscription, terminal detach, the current non-reusing event registry,
and p008's separately frozen future stale-fd reuse rule. The boot reference now
matches the common parameter parser, current required configured-loader paths,
root/overlay/swap/init consumers, and the separate old-handoff compatibility
default.

Focused validation passed: 150 live-scope Markdown links, LP64 and ILP32 evdev
layout checks, the complete q044 input-ownership runner in ordinary and
ASan/UBSan modes, BR-T42, BR-T44, and the configured-loader parser in ordinary,
sanitizer, and analyzer modes. `git diff --check` passed. Aggregate
`make check` and `.internal/` were not used. The reference/WS009 live scope
passed 150 links and the linked docs/WS003/WS013/WS016 scope passed 314. A
whole-live-tree scan remains inapplicable because an ignored WS008 `temp/`
source-copy contains a link to an intentionally absent upstream document.
