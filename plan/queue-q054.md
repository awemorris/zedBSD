# Queue: CDC NCM asynchronous TX accounting

Last updated: 2026-09-01

QID: `q054`

Queue status: completed

Queue finished: **Yes**

Authorization: the user approved the recommended `ws004-p017` asynchronous
TX-statistics contract after q053 recorded it as the remaining USB-LAN product
decision, and directed implementation.

Timebox: none. Complete the one finite NCM accounting Phase and its automatic
regressions. No physical action is required for this software-only boundary.

Parent: [master plan](master.md)

Previous Queue: [q053](queue-q053.md)

## Purpose

Make a packet accepted by CDC NCM remain counted in `tx_packets`/`tx_bytes`
while reporting a later genuine terminal USB failure exactly once through the
common network-device statistics path. Preserve `tx_dropped`, exclude orderly
administrative cancellation, and close poll versus close/detach races without
changing NCM framing or recovery policy.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p017` | [Phase](ws004-hardware/phase017-cdc-ncm-runtime-recovery/phase.md) | completed | Locked common error accounting plus exactly-once NCM terminal completion and orderly-cancel behavior pass focused, sanitizer, analyzer, build, and USB-root QEMU regressions |

## Accepted contract

- `tx_packets` and `tx_bytes` count successful driver acceptance and are not
  rolled back after a later HCD failure.
- A later `STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR` increments
  `tx_errors` exactly once.
- Such a later failure does not increment `tx_dropped`.
- `CANCELLED` caused by close, detach, or shutdown is administrative and does
  not increment an error counter.
- The common network-device helper owns locking; the NCM driver owns terminal
  status classification and exactly-once completion consumption.

## Boundaries

- Retain p020's valid-sequence/resynchronization, bounded-poll, and packet-
  filter rules without reopening them.
- Cover both ordinary poll retirement and a terminal completion observed by
  close/detach before the scheduled poll runs.
- A failed drain retains the existing graph; it neither duplicates an already
  published terminal error nor invents one for a still-pending transfer.
- Keep CDC ECM outside p017. A later consumer Phase may apply the common
  contract independently after this NCM implementation is accepted.
- Do not add notification reassembly, xHCI IRQ changes, retry policy, public
  UAPI, or vendor-specific behavior.
- Do not use `.internal/` or aggregate `make check`.

## Automatic gates

1. Extend the production `net-device.c` hotplug fixture for accepted counters,
   locked asynchronous error publication, unchanged drops, and publication
   while removal is joining driver close.
2. Extend the production-source NCM fixture for success, all four genuine
   terminal statuses, exactly-once polling, completion-before-close, orderly
   cancellation, close/open reuse, and detach ownership.
3. Run the NCM driver ordinary, ASan/UBSan, and analyzer modes; retain the NCM
   wire, p020, USB binding, concurrent-URB, removable-device, and shutdown
   regressions relevant to this change.
4. Run `make -j16`, compile configured amd64/i386 consumers, and boot a
   disposable current image through q35 xHCI USB Storage to exact `login:`.
5. Require `git diff --check`; record exact results in P/W/M/Q documents.

## Completion definition

Q054 completes when the accepted statistics contract is implemented without a
new product decision, every declared automatic gate passes or has one exact
reproducible external blocker, and `ws004-p017` is completed or honestly
`uncleared`. No physical RTL8156 observation is needed because this Queue uses
deterministic injected terminal completions and preserves the already accepted
physical NCM path.

## Execution result

`ws004-p017` completed without another product decision. The common
network-device layer now provides one locked asynchronous TX-error helper, and
CDC NCM calls it in the terminal TX callback only for `STALL`, `TIMEOUT`,
`DISCONNECTED`, and `IO_ERROR`. Driver-accepted packets/bytes remain counted,
later terminal failure changes only `tx_errors`, and close/detach/shutdown
`CANCELLED` changes neither errors nor drops. Poll/stop retains responsibility
for clearing `tx_busy` after HCD ownership is released.

The net-device hotplug fixture passes acceptance counters, locked asynchronous
publication, unchanged drops, null safety, and a removal/close join. The NCM
production-source fixture passes ordinary and ASan/UBSan runs at 2,013 checks
each plus analyzer inspection, including all four genuine statuses, exactly
once across poll/close/drain retry, orderly cancellation, reopen, detach, and
twelve fresh reconnect generations. NCM wire, USB binding (971 checks per
runtime), concurrent xHCI/USB function (1,496 checks per model runtime), USB
recovery (1,111 checks per runtime), and shutdown-order regressions pass.

The ordinary `make -j16`, forced configured amd64/i386 changed objects, and a
fresh private amd64/xHCI full image build pass. The image SHA-256 is
`0c794540d535c9a83006428683a16db4d4ffc949b457819401ce00938a7d187c`.
A disposable four-CPU, 4-GiB OVMF q35 launch booted it solely through xHCI USB
Storage, mounted the overlay root, activated swap, started init, and reached
the exact `login:` prompt in 13 seconds. No physical action was required.

CDC ECM consumption, coherent i386 statistics snapshots, dp8390 convergence,
and an autonomous NCM TX timeout policy remain explicitly separate future
boundaries and do not weaken this completed NCM result.
