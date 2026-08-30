# Queue: PC-9821V13 IPL read-contract repair

Last updated: 2026-08-31

QID: `q037`

Queue status: in-progress

Queue finished: **No**

Authorization: on 2026-08-31 the user reported that the ordinary PC-98 image
beeps and stops on a PC-9821V13, confirmed that this model ignores `55 aa`,
and explicitly requested that the suspected early-IPL defect be fixed.

Timebox: none. This Queue contains one bounded Phase. Its automatic work ends
with a frozen image for one user-operated PC-9821V13 observation.

Parent: [master plan](master.md)

Previous Queue: [q036](queue-q036.md)

## Purpose

Restore the private-stack and validated PC-98 BIOS disk-read contract in the
first native IPL stages without changing the native partition layout or
removing the cross-model `55 aa` signature. Distinguish any remaining physical
failure, preserve QEMU behavior, and produce the exact V13 handoff artifact.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p022` | [Phase](ws003-bringup/phase022-pc9821-v13-ipl-read-contract/phase.md) | in-progress | Automatic gates pass and frozen image `d2bfc9c4...` is ready; one PC-9821V13 observation remains |

## Fixed boundaries

- Retain `IPL1`, native PC-98 LBA-1 entries, the LBA-2 selector, word `9`, and
  `55 aa`. Do not add a PC/AT partition entry or GPT.
- Do not change the kernel, PC/AT/UEFI loaders, PC-98 filesystem format, or
  higher-level boot parameter contract.
- Do not consume `.internal/` or run aggregate `make check`.
- Use `make -j16`, focused source/binary/layout checks, and qemu-pc98.
- Ask for only one physical run after all automatic gates pass. A later
  boundary becomes a separately planned Phase rather than speculative retries.
- Commit and push after the Phase reaches completed or uncleared state. If
  push is unavailable, preserve the local commit and continue reporting it.

## Completion definition

q037 is finished when `ws003-p022` is completed, or uncleared with the exact
diagnostic, frozen image identity, and a concrete resume condition recorded.

## Automatic checkpoint

The private-stack/SENSE/read fix, full-cell failure diagnostics, BR-T54
source/binary/layout fixture, ordinary image checker, `make -j16`, and positive
qemu-pc98 login gate pass. The Queue is intentionally still in progress until
the one physical V13 observation reports success or one of
`1S`/`1R`/`2T`/`2N`/`2P`.
