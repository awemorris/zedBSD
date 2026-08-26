# Queue: Latitude xHCI enumeration and recovery closure

Last updated: 2026-08-26

QID: `q013`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q012](queue-q012.md)

## Purpose

Execute every presently known xHCI correction before asking for hardware
feedback. Shared-DMA synchronization is isolated in `ws003-p007`, xHCI device
association lifetime in `ws003-p008`, command/cancellation ownership in
`ws003-p005`, Halted/Normal-TD recovery in `ws003-p006`, SuperSpeed endpoint
context in `ws003-p009`, and Control TD/root-port reset/EP0 enumeration in
`ws003-p004`. No completed or unrelated Phase is reopened. Freeze one
production image after all six Phases and all host/build/QEMU gates pass, then
request exactly one Latitude BR-T34 boot and feed that same observation back
to every affected plan record.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p007` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase007-shared-dma-allocation-synchronization/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T36 passed and BR-T34 exercised both physical xHCI controllers without DMA-registry failure |
| 2 | `ws003-p008` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase008-xhci-device-association-lifetime/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T37 passed and BR-T34 configured multiple devices and registered the intended storage object |
| 3 | `ws003-p005` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase005-xhci-command-cancel-lifecycle/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T28/BR-T29 passed; BR-T34 showed no command/cancel/retained-DMA failure |
| 4 | `ws003-p006` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase006-xhci-halted-endpoint-recovery/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T35 passed and BR-T34 reached BOT/SCSI through working bulk endpoints |
| 5 | `ws003-p009` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase009-superspeed-endpoint-context/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T38 passed and the SuperSpeed boot medium configured and registered as `sda` |
| 6 | `ws003-p004` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase004-latitude-xhci-device-enumeration/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T34 reached `usb-storage: sda`; U2 passed and the later U3 SCSI flush stop was extracted |

## Phase aggregation decision

- The initial audit found all known work collected in p004. q013 separates the
  independently testable P1 command/cancel lifecycle into p005 so it can close
  before p004 re-enters the failing enumeration path.
- Review during q013 exposed two independent boundaries which must precede the
  same physical run: xHCI Halted-endpoint recovery (`p006`) and the shared
  PCI-root DMA allocation-list race exercised by the Latitude's two xHCI
  functions (`p007`). They are added to q013 rather than deferred or tested on
  hardware separately.
- Continued integrated review added the multi-device association lifetime
  (`p008`) and SuperSpeed Slot/Endpoint Context (`p009`) boundaries. These are
  also completed before, and consume, the same BR-T34 run.
- `ws003-p003` is Partial but its remaining implementation was transferred to
  p004; re-queueing it would duplicate work. The final BR-T34 observation is
  still written back to its handoff evidence.
- `ws004-p004` is a completed QEMU milestone. Its SuperSpeed and fault-path
  follow-ups overlap p004 D1--D3 and p005 command/cancel work, so it receives
  cross-evidence instead of being reopened.
- WS004 USB-root manual acceptance and WS006 USB HID are downstream scopes and
  are not known causes of the current pre-storage EP0 failure.
- If implementation exposes a genuinely independent problem, add a new Phase
  to this Queue before the one physical run only when its scope and automated
  acceptance can be completed safely. Do not ask for an intermediate boot.

## Ordered execution

1. p007 BR-T36: synchronize the shared DMA allocation registry across IRQ and
   SMP allocation/free/map/destroy paths.
2. p008 BR-T37: replace lockless xHCI device-list lookup with a USB
   lifecycle-owned direct association.
3. p005 BR-T28/BR-T29: correct and fault-inject endpoint state,
   cancellation/request/DMA ownership, Disable Slot, Set TR Dequeue, and
   Command Completion matching.
4. p006 BR-T35: recover EP0 and bulk endpoints after STALL and fix split
   Normal IN event/TD Size semantics.
5. p009 BR-T38: preserve raw speed and SuperSpeed bulk companion fields.
6. p004 BR-T27: correct and fixture Control TRBs, port-reset state, EP0 packet
   size, Control Average TRB Length, and address recovery interval.
7. Add bounded stage/completion diagnostics, then run all applicable WS003 and
   WS004 host regressions.
8. Run `make -j16`, `git diff --check`, legacy-BIOS xHCI USB root, and BR-T24
   OVMF USB root at 4, 8, and 16 GiB. Do not use `make check` or `.internal/`.
9. Freeze and hash exactly one `build/amd64/hdd-image.img` candidate.
10. Request BR-T34 once on the Latitude. No earlier physical confirmation and
   no repeated intermediate boot are permitted.
11. Feed that single result into `ws003-p004` through `ws003-p009`, the
   `ws003-p003` handoff, WS003/M2, WS004 xHCI physical/fault coverage, and the
   shared evidence.

## Safety and scope

- The q012 artifact is diagnostic-only because its failed-cancel path can lose
  request/DMA ownership. Do not request or perform another physical boot with
  it.
- Do not free a request, bounce buffer, transfer ring, context, or slot while
  the controller may still DMA. Unrecoverable state is quiesced and retained.
- Do not mask failures with unconditional retries or fixed sleeps.
- Do not implement USB HID, storage-class, VFS, NVMe, WLAN, graphics, or broad
  multi-device scheduling unless a new bounded Phase is explicitly added.
- Do not commit changes.

## Execution record

q013 started on 2026-08-26. p004--p009 are aggregated before a single shared
physical gate. The implementation and final review found no remaining P0/P1
in the Latitude xHCI USB-mass-storage boot path. Generic USB topology and
driver-registry lifetime, retained-object reaping, and future IOMMU mapping
lifetime remain documented debt outside this boot-path claim.

Automatic results for the frozen candidate:

- BR-T25--BR-T29 and BR-T35--BR-T38 focused coverage: PASS;
- DMA constraints, xHCI model, USB URB publication, and USB-storage SCSI
  regressions: PASS;
- BR-T29 QEMU auxiliary xHCI device remove/re-add with a live IDE root: PASS;
- legacy BIOS q35/xHCI USB-only root through `login:`: PASS;
- BR-T24 OVMF q35/xHCI USB root at 4, 8, and 16 GiB: PASS 3/3, with
  `RSDP=0x000000007f77e014` in every case;
- `make -j16`, shell syntax checks, and `git diff --check`: PASS.

Frozen image:

- [build/amd64/hdd-image.img](../build/amd64/hdd-image.img)
- 135266304 bytes
- SHA-256
  `bd3aa801ac890deabb5f0ad4b6f3388e5137992e9f6f81e8d912af4abad7585f`

BR-T34 was performed once on the Latitude with this artifact. Both physical
xHCI controllers remained attached; ports 8, 10, and 13 reset and configured,
and the SuperSpeed boot medium registered as `usb-storage: sda`. UUID
`45a3-2251` resolved to `/dev/sda1`; `rootfs.img` and the read-write
`data.img` were attached as `loop0` and `loop1`. No EP0, xHCI command,
endpoint-recovery, DMA-retention, enumeration, or boot-storage-timeout error
recurred. This proves U2 and completes p004--p009.

The first later stop is U3: the writable-overlay mount flush issued SCSI
SYNCHRONIZE CACHE(10), and the medium returned CHECK CONDITION with
`sense=05/20/00` (Illegal Request / Invalid Command Operation Code). The BOT
transport, CSW, REQUEST SENSE, disk discovery, partition scan, and UUID
resolution all completed before that bounded failure. This independent
optional-command/cache-capability boundary is extracted to `ws003-p010` and
does not reopen the six completed xHCI Phases.

q013 is finished with every Queue item completed. Repeatability remains the
later BR-T30 five-boot acceptance and was not requested at this boundary.
