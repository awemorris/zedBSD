# Queue: xHCI SuperSpeed interrupt endpoint context

Last updated: 2026-08-31

QID: `q045`

Queue status: finished

Queue finished: **Yes**

Authorization: after the current corrections, the user directed continuous
execution of the remaining workstreams. `ws004-p021` is a fully designed,
independent standards-correction Phase with no unresolved implementation
decision. Its one physical checkpoint may be deferred without blocking later
automatic work.

Timebox: none. Complete the automatic/source boundary, record any unavailable
image or physical gate honestly, synchronize P/W/M, commit locally, and select
the next dependency-ready Queue.

Parent: [master plan](master.md)

Previous Queue: [q044](queue-q044.md)

## Purpose

Make native xHCI endpoint contexts truthful for SuperSpeed interrupt
endpoints. Decode and validate the retained companion descriptor before ring
allocation, then encode Max ESIT Payload and Average TRB Length without
changing non-SuperSpeed or non-interrupt contexts.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p021` | [Phase](ws004-hardware/phase021-xhci-superspeed-interrupt-context/phase.md) | uncleared | Automatic/source milestone passed: exact RTL8156 words, pre-DMA rejection, USB/xHCI/NCM/storage regressions, analyzer, and configured x86 objects pass. The existing Noct `--path` verifier mismatch prevents a fresh image, so QEMU and Latitude checkpoints remain |

## Fixed boundaries

- Add one typed read-only companion accessor; keep the existing burst accessor.
- Validate every SuperSpeed interrupt field before allocating a transfer ring.
- Preserve control, bulk, isochronous, and FS/LS/HS interrupt context bytes.
- Do not add VID:PID quirks, NCM-specific HCD behavior, public UAPI, or wider
  periodic/isochronous redesign.
- Use Phase-owned focused fixtures, sanitizers/analyzer, configured builds, and
  disposable QEMU storage only. Do not use `make check`, `.internal/`, or Noct
  source changes.

## Completion definition

q045 finishes when the Phase is processed through all locally controlled
source, model, build, and runtime gates. If the known Noct image gate prevents
QEMU, or the one Latitude carrier/DHCP/fetch observation is unavailable, retain
the automatic milestone and mark only the remaining Phase boundary
`uncleared`; then continue to the next Queue.

## Result

The Queue finished with `ws004-p021` uncleared only at its fresh-image runtime
boundary. The Phase-owned ordinary, ASan/UBSan, analyzer, source-order, amd64,
i386, USB function, xHCI, concurrent-URB, binding, NCM, and USB-storage gates
all passed. The configured `make -j16` reached the existing host-Noct verifier,
which rejected `--path=tools/build`; the separately linked `vmunix` was deleted
by the same failed check. No stale image was substituted, so disposable QEMU
and the one Latitude check remain pending. Exact commands and counts are in
the Phase book.
