# Queue: Intel Mac automatic image groundwork

Last updated: 2026-08-31

QID: `q036`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-30 the user inserted the Intel Mac bring-up
workstream immediately after WS018 and directed autonomous Queue execution.
On 2026-08-31 the user removed the target-medium-capacity selector and fixed
the pure-Protective-MBR UEFI-only image policy.

Timebox: none. The physical Intel Mac checkpoint was outside this Queue.

Parent: [master plan](master.md)

Previous Queue: [q035](queue-q035.md)

Next Queue: [q037](queue.md)

## Purpose

Introduce the generic Architecture -> Board -> Variant configuration,
implement the three amd64 disk layouts without changing kernel or loader
compilation, and attempt the strict six-cell SeaBIOS/OVMF acceptance matrix.

## Final execution registry

| Priority | WS / Phase | Authoritative document | Final state | Result |
| --- | --- | --- | --- | --- |
| 1 | `ws020-p001` | [Phase](ws020-intel-mac/phase001-target-variant-config/phase.md) | completed (revised) | Generic board-owned Variant and three-way artifact invariance pass; the withdrawn capacity field is absent |
| 2 | `ws020-p002` | [Phase](ws020-intel-mac/phase002-image-layouts/phase.md) | completed (revised) | Combined, BIOS-only, and fixed pure-Protective-MBR UEFI-only layouts pass strict image/GPT gates |
| 3 | `ws020-p003` | [Phase](ws020-intel-mac/phase003-qemu-acceptance/phase.md) | uncleared | Maintained six-cell runner and strict positive/negative oracles are complete, but three fresh runs exposed a pre-existing intermittent init/getty stall after storage, root overlay, and swap succeeded |

## Uncleared handoff

The p003 runtime stop crossed SeaBIOS/OVMF, image layouts, SMP/uniprocessor,
and multi-/single-thread TCG. Attempts `-004`, `-005`, and `-006` stopped at
`boot: starting init /sbin/init` or after `getty_console` plus
`init: system running`, without the required `login:` prompt. The strict
oracle was not weakened and retries were not converted into acceptance.

Resume `ws020-p003` only after the separately planned runtime/init/getty flake
has a diagnosis or fix, then require one fresh uninterrupted six-cell pass.
`ws020-p004` remains blocked behind that result.

## Closure

q036 is finished because every selected item is either completed or uncleared
with evidence and a concrete resume condition. It does not complete WS020.
