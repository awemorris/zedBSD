# Queue: USB-storage flush capability and U3 root continuity

Last updated: 2026-08-27

QID: `q014`

Queue status: finished

Queue finished: **Yes**

Archived Queue record: `plan/queue-q014.md`

Authorization: explicitly approved by the user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q013](queue-q013.md)

## Purpose

Execute only `ws003-p010`. The one q013 BR-T34 Latitude boot proved xHCI U2
and then stopped at U3 because the USB medium rejected optional SCSI
SYNCHRONIZE CACHE(10) with `sense=05/20/00`. Discover the medium's caching
contract through bounded MODE SENSE data, choose a fail-safe flush/FUA policy,
retain precise diagnostics, pass host/build/QEMU gates, and freeze one image
for exactly one later Latitude confirmation.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p010` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase010-usb-storage-flush-capability/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | completed | BR-T41 passes: the USB-backed writable overlay, init/login, root shell, and X/`zterm` are reached without the opcode-35 failure |

## Entry evidence and decision

- q013 is finished. Its single BR-T34 run configured ports 8, 10, and 13,
  registered `usb-storage: sda`, resolved UUID `45a3-2251` to `/dev/sda1`, and
  attached both loop images without an xHCI, enumeration, or storage-discovery
  failure.
- The first failure was a successful BOT command exchange ending in CHECK
  CONDITION for opcode `0x35`, followed by valid REQUEST SENSE
  `05/20/00`. This is an unsupported-command/caching-policy boundary rather
  than a recurrence of q013's xHCI defects.
- SCSI SYNCHRONIZE CACHE(10) is not universally implemented. Treating every
  unsupported flush as success would falsely promise stable storage; treating
  it as fatal even when caching is proved disabled needlessly rejects valid
  direct-access media. p010 owns this distinction.

## Ordered execution

1. Add BR-T39 production-shared fixtures for MODE SENSE(6) header/page parsing,
   malformed and truncated data, WCE/DPOFUA combinations, unsupported-command
   sense classification, BOT status/residue accounting, and the resulting
   flush/write policy.
2. Probe the Caching mode page with a bounded buffer while preserving the
   existing write-protect result. Record whether cache state and FUA support
   are known; never infer a safe no-op from missing or malformed data.
3. Before `disk_create()` publishes the disk, select proved write-through
   directly or preflight SYNCHRONIZE CACHE, then fix one immutable write/flush
   policy. A proved FUA-capable fallback puts FUA on every WRITE(10) from the
   first write; unknown or unsafe combinations publish the disk read-only.
4. Cache only stable capability outcomes. Preserve the original sense and emit
   one bounded diagnostic naming `sync-cache`, `write-through`, `fua`, or
   `unsafe/unknown`, rather than the misleading partition-offset LBA for a
   whole-device flush.
5. Run BR-T39, the existing USB-storage SCSI, USB URB, xHCI, DMA, and applicable
   filesystem regressions; run `make -j16` and `git diff --check`. Do not use
   `make check` or `.internal/` material.
6. Boot disposable copies of the production image through legacy BIOS and
   OVMF q35/xHCI USB root. Require writable-overlay mount and `login:` without
   hiding injected unsafe-cache failures.
7. Freeze, size, and hash one `build/amd64/hdd-image-q014.img`, then request
   exactly one BR-T41 Latitude boot. Do not request intermediate physical runs.
8. Feed the one physical result into p010, WS003 U3/M2, and the shared evidence.
   Final repeatability remains BR-T30 after U4 is otherwise frozen.

## Safety and scope

- Do not report a flush success solely because opcode `0x35` returned
  `05/20/00`.
- Do not enable a FUA fallback unless the MODE SENSE header advertises DPOFUA.
  Select it during probe before disk publication so every write carries FUA;
  switching after an ordinary write has completed is forbidden.
- Unknown, contradictory, short, or malformed cache data is fail-safe and
  forces read-only publication.
- Loss of a previously working runtime SYNCHRONIZE CACHE path is a sticky I/O
  failure. Do not fall back to FUA or write-through after non-FUA writes may
  already be dirty.
- Do not alter xHCI, BOT phase ownership, VFS/overlay semantics, image layout,
  or unrelated USB classes unless new evidence proves that boundary and a new
  Phase is authorized.
- Use disposable image copies for runtime write tests and do not commit.

## Execution record

q014 started on 2026-08-26. Phase design, implementation, P0/P1 review, and all
automatic gates are complete. BR-T39 plus eight related host regressions,
`make -j16`, `git diff --check`, legacy BIOS USB root, the QEMU WCE-clear
write-through/no-op path, BR-T24 OVMF USB root at 4/8/16 GiB, BR-T29 hotplug,
and a final pristine-copy USB-overlay boot all pass. The WCE-clear QEMU command
log contains no SYNCHRONIZE CACHE command. `make check` was not run and
`.internal/` was not used.

The single BR-T41 artifact is
historical generated artifact `build/amd64/hdd-image-q014.img`, 135266304
bytes, SHA-256
`003b54ef77e1fe2e0d96278421441ff7cf4988f736f766f433bf33d6b11cd891`.
BR-T41 was performed once on 2026-08-27 with the requested frozen q014 image.
The user-provided photographs show `/dev/sda1` resolution, the private
read-write `loop1`, both UFS checks, the root overlay, runtime mounts, init and
services, root login and shell, and X/`zterm`. The prior opcode-35/BOT/VFS
failure does not recur. The exact cache-policy diagnostic tuple is not visible
in the photographs and is not inferred; BR-T39 and the frozen fail-closed
implementation remain its automatic policy evidence.

Result: BR-T41 **PASS**. q014 is finished with every Queue item completed. No
next Queue is authorized here. Final five-boot repeatability remains BR-T30,
and sustained physical root I/O remains BR-T31.
