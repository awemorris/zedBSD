# Queue: QEMU NVMe and strict GPT foundation

Last updated: 2026-08-30

QID: `q030`

Queue status: completed

Queue finished: **Yes — 2026-08-30**

Authorization: after reviewing and refining the staged NVMe overlay installer
plan, the user explicitly requested implementation on 2026-08-29. This Queue
is the first dependency-closed implementation slice.

Timebox: no fixed wall-clock limit. Complete or honestly mark uncleared each
finite software Phase. No physical Latitude action is requested in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q029](queue-q029.md)

## Purpose

Implement and accept the native NVMe/block/GPT substrate required by every
later loader and installer Phase. Keep this Queue within disposable QEMU and
focused fixtures so that UEFI/installer work does not grow around an unproven
storage driver.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p022` | [Phase](ws004-hardware/phase022-nvme-admin-identify/phase.md) | completed | Standard PCI NVMe reset/admin queues/Identify publish exactly one truthful discovery-only `/dev/nvme0n1`; focused, build, QEMU, IDE, and USB gates pass |
| 2 | `ws004-p023` | [Phase](ws004-hardware/phase023-nvme-io-lifecycle/phase.md) | completed | One bounded I/O queue provides read/write/flush and safe timeout/reset/shutdown/detach; focused/build/QEMU/IDE/USB gates pass |
| 3 | `ws004-p024` | [Phase](ws004-hardware/phase024-nvme-qemu-acceptance/phase.md) | completed | Disposable QEMU integrity/lifecycle and strict primary/backup GPT partition gates pass |

Final result: p022 through p024 completed on 2026-08-30. The retained
[I/O evidence](ws004-hardware/tests/q030-nvme-io-evidence.md) and
[strict-GPT evidence](ws004-hardware/tests/q030-nvme-gpt-evidence.md) cover
ordinary/sanitizer/analyzer fixtures, amd64 and i386 PC/AT builds, disposable
5-GiB raw and GPT QEMU namespaces, below/above-4-GiB descriptor flush and
restart readback, SQ1/CQ1 wrap, four-worker concurrency, malformed-GPT
fail-closed rejection, and IDE plus xHCI USB-root regressions.

Next dependency-ready Queue: `q031`, grouping `ws013-p002`--`p003` followed by
`ws019-p002`--`p005`. This keeps UEFI payload discovery, bounded `boot.cfg`
translation, read-only disk administration, preflight, copy transaction, and
QEMU installer acceptance together before the single physical
`ws004-p025`/`ws003-p018` checkpoint.

## Dependency order

```text
ws004-p022 controller/admin/Identify/read-only namespace
                         |
                         v
ws004-p023 I/O queue/read/write/flush/lifecycle
                         |
                         v
ws004-p024 strict GPT + disposable QEMU acceptance
                         |
                         v
future q031: WS013 p002/p003 and WS019 p002-p005
```

## Frozen boundary

- Match PCI class `01/08/02`, not a vendor ID.
- Support one controller, one active 512-byte namespace, one admin queue, one
  I/O queue, and one MSI-X/MSI message.
- Use coherent queue memory and bounded coherent bounce buffers; do not claim
  scatter/gather, IOMMU isolation, multipath, hotplug, or multiple I/O queues.
- Publish `/dev/nvme0n1`; retain one-based partition names.
- p022 publishes discovery metadata only: generic writes reject with `EROFS`
  and reads report `EOPNOTSUPP`. p023 adds NVM reads, writes, and flush only
  after queue lifecycle tests pass.
- p024 adds a strict read-only GPT scanner. It does not add a GPT writer,
  formatter, rescan UAPI, installer, or bootloader behavior.
- Physical SN740 work (`ws004-p025`) and every WS013/WS019 implementation are
  outside q030. If q030 succeeds, construct the next dependency-ready Queue
  automatically under the user's implementation authorization.

## Execution rules

- Do not inspect or modify `.internal/` or `userland/noct/NoctLang`.
- Preserve unrelated work and stage only q030 files at Phase boundaries.
- Use `make -j16`, focused WS004 fixtures, and `qemu-system-x86_64`; do not use
  `make check`.
- All block writes use disposable images and explicitly stay outside the
  source/system image.
- Run ordinary, sanitizer, and analyzer variants where the Phase fixture
  supports them. Record unavailable host facilities rather than replacing a
  required runtime with an assertion-only model.
- After each completed Phase, synchronize P/W/M/Q, commit `WIP`, and push. If
  push is unavailable, keep the local commit and continue.
- A newly discovered human product/safety decision makes only the affected
  Phase `uncleared`; record it and continue any independent Queue item.

## Completion definition

q030 is finished when p022--p024 are each `completed` or honestly `uncleared`,
all declared focused/build/QEMU evidence is recorded in P/W/Q, and the master
identifies the exact next dependency-ready Queue or human decision.
