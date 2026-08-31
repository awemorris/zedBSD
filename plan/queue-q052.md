# Queue: xHCI SuperSpeed interrupt-context runtime closure

Last updated: 2026-09-01

QID: `q052`

Queue status: completed

Queue finished: **Yes**

Authorization: after q051 was committed, synchronized with origin, and pushed,
the user directed automatic continuation into USB LAN work. The independent
CDC NCM physical path and CDC ECM QEMU baseline are already complete. This
Queue therefore selects the remaining dependency-ready USB-LAN/xHCI runtime
slice instead of duplicating either class driver.

Timebox: none. Process the automatic/runtime boundary of the one selected
Phase. The existing final Latitude observation is a physical evidence boundary,
not a new product decision; if it is unavailable, record the Phase as
`uncleared`, finish this Queue, and allow later Queues to continue.

Parent: [master plan](master.md)

Previous Queue: [q051](queue-q051.md)

## Purpose

Consume q047's repaired host-Noct verifier path for the already implemented
SuperSpeed interrupt endpoint correction. Revalidate the current source,
construct one fresh amd64/UEFI image in an empty private build tree, boot it as
the only xHCI USB system disk to `login:`, and freeze that exact image for one
later Latitude/RTL8156 checkpoint.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p021` | [Phase](ws004-hardware/phase021-xhci-superspeed-interrupt-context/phase.md) | uncleared | Current-source focused/regression/configured-build gates and one fresh OVMF q35+xHCI USB-root boot pass; retain one hash-pinned candidate for the still-required Latitude `ue0` checkpoint |

## Dependencies and exclusions

- `ws004-p010`, p011, p014, and p018 are complete.
- q047 restored the repository's supported Noct `--path` verifier invocation.
  The separate Noct compile/application parser defect does not gate this Queue.
- q029 proved the earlier RTL8156 NCM data path, but predates the p021 endpoint
  context correction and cannot substitute for the final patched-image check.
- This Queue does not reimplement ECM or NCM, change NCM framing, resolve
  p017's asynchronous TX-accounting policy, add a VID:PID quirk, or begin WLAN.
- QEMU does not emulate the RTL8156 SuperSpeed notification endpoint. Its role
  here is fresh-image integration and xHCI USB-root regression, not physical
  Max-ESIT proof.

## Fixed boundaries

- Retain the frozen p021 descriptor validation and endpoint-context encoding:
  RTL8156 words `0x000a0000`, `0x0010003e`, and `0x00100010`; validation
  precedes transfer-ring allocation.
- Run the Phase-owned ordinary, ASan/UBSan, analyzer, production-source, and
  configured-object gates plus retained concurrent-URB, USB binding, NCM
  wire/driver, Storage, and URB-publication regressions.
- Build in an empty private directory from current source. Do not reuse a
  pre-q045 or differently configured disk image.
- Run one bounded OVMF/q35/xHCI USB-storage-only system boot from a disposable
  copy. Require exact `login:`, required amd64/xHCI/Storage markers, no fatal
  marker, and an unchanged source-image hash.
- Preserve one read-only candidate under `build/` with its SHA-256 for a
  single later physical check. Do not perform remote or physical-machine
  mutation automatically.
- Do not use `.internal/` or aggregate `make check`.

## Completion definition

q052 finishes when all automatic gates are either complete or have one exact
reproducible blocker and the physical handoff is explicit. The Phase itself
becomes complete only after the same hash-pinned candidate boots once on the
Latitude 5320 with truthful `ue0` carrier, DHCP, peer ping, external fetch,
and no freeze. An unavailable physical observation leaves the Phase
`uncleared` without blocking the next Queue.

## Execution result

The complete automatic/runtime milestone passes without a new product
decision. A commit-time standards audit found and corrected one overly broad
validation bound before the final image was accepted.

- The strict endpoint-context fixture passes 82 checks in ordinary and
  ASan/UBSan modes. The current production USB function fixture passes 1,496
  checks in both modes; the xHCI model, source-order check, analyzer builds,
  and amd64/i386 configured production objects pass.
- SuperSpeed Interrupt Max ESIT payload is now capped at the architectural
  3-KiB limit rather than the SuperSpeed Isochronous 16-KiB limit. The focused
  fixture accepts 3,072 bytes, rejects 3,073 bytes with descriptor capacity
  deliberately still available, and continues to encode the RTL8156 16-byte
  case exactly. The pre-audit image was discarded.
- Concurrent xHCI URBs, USB binding (971 checks in each runtime mode), NCM wire,
  and the integrated NCM driver (1,540 checks in each runtime mode) pass their
  retained gates. Storage-SCSI and URB-publication models pass ordinary,
  ASan/UBSan, and analyzer compilation.
- A completely new private amd64/UEFI build passes with the p021 xHCI/NCM
  configuration after that correction. One 4-GiB, four-CPU OVMF q35 launch
  boots that image solely through xHCI USB Storage to `login:` with no fatal
  marker; RSDP is `0x000000007f77e014` and the source hash remains unchanged.
- `git diff --check` passes. The final QEMU evidence is
  `/tmp/ws004-q052-final-002/qemu`.

Latitude candidate: `build/ws004-p021-q052-hdd-image.img`

SHA-256:
`43f3ee1165a0bd4b719df5eea1a3b4d54b8b2c2655f5267cb1581e9b84099bde`

The candidate is read-only on the host. One physical Latitude 5320 boot remains;
therefore `ws004-p021` is honestly `uncleared`, while q052 itself is
finished.
