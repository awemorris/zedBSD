# WS003 Phase 010: USB-storage flush capability

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p010`

Combined ID: `ws003-p010`

Status: Complete (`q014`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Move the Latitude USB boot from proven U2 to U3 by representing the actual
SCSI write-cache durability contract. A medium which implements SYNCHRONIZE
CACHE(10), is proved write-through, or advertises a usable FUA path can satisfy
`BIO_FLUSH`; an unknown or unsafe contract fails visibly rather than silently
claiming durable writes.

## Baseline and confirmed boundary

The one q013 BR-T34 boot completed the xHCI and storage-enumeration path:

```text
xhci: port 13 reset complete portsc=00201203
usb1: device 4 port 13 30de:6544 class 00 configured
usb-storage: sda blocks=60549120 block-size=512
vfs: boot selector UUID=45a3-2251 resolved to /dev/sda1
vfs: loop0 <- rootfs image (private, read-only)
vfs: loop1 <- data.img (private, read-write)
```

The first subsequent stop occurred when the writable overlay needed a durable
flush:

```text
usb-storage: BOT check-condition residue=0
usb-storage: sda op=35 lba=2048 blocks=0 error=5 sense=05/20/00
vfs: mount root overlay failed (error 5)
VFS initialization failed (5); entering idle.
```

Sense `05/20/00` is Illegal Request / Invalid Command Operation Code. The BOT
CSW and REQUEST SENSE path worked; opcode `0x35` was rejected by the medium.
The displayed `lba=2048` is the partition mapping offset inherited from the
BIO diagnostic, not an LBA encoded by this whole-device flush CDB. This is a
distinct SCSI capability-policy defect and does not reopen p004--p009.

## Confirmed implementation defect

At q014 entry, `drivers/usb-storage.c` requested only the four-byte MODE
SENSE(6) header using page code `0x3f`. That was enough to read the WP bit but
could not parse the Caching mode page or its WCE bit. The driver therefore
always sent
SYNCHRONIZE CACHE(10) for `BIO_FLUSH` and collapsed a valid unsupported-command
response to `EIO`, without knowing whether the device is already write-through
or can make each WRITE(10) durable with FUA.

## Scope

- Decode a bounded MODE SENSE(6) response, including Mode Data Length,
  Device-Specific Parameter WP/DPOFUA bits, Block Descriptor Length, and the
  Caching mode page's WCE bit.
- Validate lengths before every header, block-descriptor, and page access;
  reject malformed, truncated, duplicate, or contradictory cache data as
  unknown.
- Validate BOT CSW residue against the requested and actual data length. Keep
  SCSI CHECK CONDITION distinct from transport/phase failure, recover a data
  STALL far enough to read its CSW, and request/classify sense only for a
  validated command-failed CSW.
- Accept `05/20/00` as capability evidence only from a bounded current-error
  fixed or descriptor sense response. Deferred responses and data truncated
  before the required header/ASC fields remain diagnostic failure; truncation
  of optional bytes beyond the requested allocation is permitted.
- Keep write protection independent of cache discovery: a usable header may
  still establish WP even if the cache page is absent or malformed.
- Before `disk_create()` publishes the device, fix one per-device write/flush
  policy. A positively disabled cache selects write-through directly;
  otherwise preflight SYNCHRONIZE CACHE. Do not discover or switch durability
  mode after ordinary writes may have completed.
- Preserve SYNCHRONIZE CACHE(10) as the preferred `BIO_FLUSH` operation when
  volatile-cache state is enabled or unknown and the preflight succeeds.
- Classify only the precise unsupported-command sense (`05/20/00`) as evidence
  that opcode `0x35` is unavailable. Transport errors, phase errors, timeout,
  Unit Attention, Not Ready, and other sense values remain errors.
- Permit a successful no-op flush only when WCE is positively known to be
  clear, or when the advertised-DPOFUA policy was selected during probe and
  every WRITE(10), beginning with the first, carries FUA.
- Publish an unknown or unsafe cache contract as read-only. If a preflighted
  SYNCHRONIZE CACHE path later fails, make that failure sticky; never switch to
  FUA or write-through after non-FUA data may already be dirty.
- Emit bounded policy diagnostics and preserve the failing command/sense.
- Preserve q013 xHCI, QEMU USB-root, and fail-closed storage behavior.

## Non-goals

- Assuming all USB flash devices are write-through merely because opcode
  `0x35` is optional or unsupported.
- Issuing FUA when DPOFUA is not advertised, or silently clearing a failed FUA
  bit and retrying a write without durability.
- MODE SELECT cache changes, disabling a device cache, SYNCHRONIZE CACHE(16),
  READ/WRITE(16), command queuing, UAS, or performance tuning.
- Changing BOT/xHCI completion ownership, overlay/UFS consistency semantics,
  the loop layout, or the block-layer meaning of `BIO_FLUSH`.
- Requesting more than one physical boot before the later BR-T30 final
  repeatability campaign.

## Dependencies

- q013 and `ws003-p004`--`p009` are complete; BR-T34 proves the physical U2
  boundary and supplies this Phase's exact medium response.
- The existing USB-storage sense parser, HW-T12 storage model, URB/xHCI
  regressions, and disposable QEMU USB-root harnesses.
- One user-operated Latitude boot only after the q014 artifact is frozen.

## Fixed policy matrix

| Probe result before disk publication | Published write mode | `BIO_FLUSH` behavior |
| --- | --- | --- |
| WCE is positively known clear | read-write, ordinary WRITE(10) | success as `write-through`; no command needed |
| SYNCHRONIZE CACHE preflight succeeds | read-write, ordinary WRITE(10) | issue SYNCHRONIZE CACHE; any later failure becomes sticky |
| Preflight returns exact `05/20/00`, WCE known set, DPOFUA advertised | read-write, WRITE(10) with FUA from the first write | success as `fua`; no non-FUA write is permitted |
| Preflight returns exact `05/20/00`, WCE known set, DPOFUA clear | read-only | reject writes; durability cannot be honored |
| Cache state unknown or MODE SENSE malformed, but preflight succeeds | read-write, ordinary WRITE(10) | issue SYNCHRONIZE CACHE; its success is the durability basis |
| Cache state unknown/MODE SENSE malformed and preflight is unsupported, or preflight has another failure | read-only | reject writes; no safety inference |

Any required preflight occurs in `scsi_probe()` before `disk_create()`; a
proved WCE-clear result needs no redundant command. An exact unsupported-command
result is usable only after valid sense decoding. The selected mode is
immutable for the disk lifetime: in particular, a runtime sync failure cannot
safely fall back to FUA because earlier ordinary writes may still reside in
volatile cache.

## Expected files and subsystems

- `drivers/usb-storage.c`
- `src/kern/vfs.c` (failure-stage diagnostic only)
- `include/drivers/usb-storage-bot.h`
- `include/drivers/usb-storage-scsi.h`
- `plan/ws003-bringup/tests/usb-storage-flush-test.c`
- applicable existing WS004 USB-storage/xHCI fixtures and QEMU evidence
- this Phase, WS003/M2, q014, and shared physical evidence

## Ordered work packages

- [x] Add BR-T39 exact fixtures for valid MODE SENSE(6) header + Caching page,
      nonzero block-descriptor length, WCE/DPOFUA/WP combinations, absent and
      duplicate pages, short/truncated data, and invalid declared lengths.
- [x] Add production-shared cache/sense policy helpers and prove the fixed
      matrix, including that only `05/20/00` selects unsupported opcode.
- [x] Add production-shared BOT status/residue helpers. Prove short IN and
      partial OUT accounting, invalid residue rejection, and that only a valid
      command-failed CSW can trigger REQUEST SENSE classification.
- [x] Replace the four-byte probe with a bounded Caching-page request and keep
      independently validated WP, WCE, and DPOFUA state in `usb_storage`.
- [x] Select proved write-through directly, or preflight SYNCHRONIZE CACHE
      inside `scsi_probe()` before `disk_create()`, then immutably select
      supported sync or advertised FUA-from-first-write. Publish unsafe/unknown
      combinations read-only.
- [x] Make runtime loss of a supported sync path sticky. Keep returning the
      original failure and never downgrade to FUA/write-through after an
      ordinary write may have completed.
- [x] Add concise diagnostics naming the selected policy and report a
      whole-device flush without the misleading partition-offset LBA/count.
- [x] Pass BR-T39, existing USB-storage SCSI, USB URB, xHCI, DMA, and applicable
      filesystem host regressions.
- [x] Run `make -j16` and `git diff --check`; do not run `make check` or consume
      `.internal/` material.
- [x] Boot disposable image copies through legacy BIOS and OVMF q35/xHCI USB
      root. Require U3, init, and `login:` on the safe policy path; fault
      fixtures must still reject an unknown/unsafe cache contract.
- [x] Freeze the production image, record its exact path, size, and SHA-256,
      then request BR-T41 once on the Latitude.
- [x] Feed the single BR-T41 result into this Phase, WS003/M2, q014, and the
      shared evidence. Defer five-run repeatability to BR-T30.

## Acceptance cases

- `BR-T39`: the production-shared host fixture proves bounded MODE SENSE(6)
  parsing and the entire fixed policy matrix. It rejects malformed/unknown
  state, validates BOT status/residue accounting, does not mistake another or
  deferred Illegal Request for unsupported opcode, publishes unsafe media
  read-only, proves FUA is present on the first write, and proves a runtime
  sync failure preserves its first error rather than switching mode.
- Existing USB-storage SCSI, USB URB publication, xHCI, DMA, and filesystem
  regressions pass.
- Legacy BIOS and OVMF q35/xHCI boots from disposable production-image copies
  mount the writable overlay and reach `login:` without a BOT, SCSI, loop,
  UFS, or VFS error.
- `BR-T41`: one Latitude cold boot of the frozen q014 image reaches at least
  U3: `/dev/sda1` resolves, `loop1` mounts read-write, the overlay mounts, and
  init starts without an unhandled opcode-35 `05/20/00` failure or a
  false-success cache policy. A later independent U4 stop is recorded but does
  not erase U3.

## Completion conditions

- MODE SENSE parsing is bounded and explicitly distinguishes known-disabled,
  known-enabled, and unknown cache state.
- Every read-write disk has one immutable pre-publication durability basis:
  `sync-cache`, `write-through`, or `fua`; unsafe/unknown media is read-only.
- Unsupported, unknown, and unsafe paths remain visible and deterministic;
  neither VFS nor the block layer is weakened to mask them.
- BR-T39, relevant regressions, QEMU USB-root, build, and diff checks pass.
- The one BR-T41 Latitude observation reaches U3 and is recorded. Final
  five-boot repeatability remains BR-T30.

## Actual results and evidence

q014 was authorized on 2026-08-26. Planning, implementation, review, and every
automatic gate are complete. The driver now parses actual MODE/SENSE transfer
lengths, validates BOT CSW status/residue, obtains sense only from a validated
command-failed CSW, and fixes one durability mode before disk publication.
Known WCE-clear media uses write-through without opcode `0x35`; other writable
media uses a proven sync path or advertised FUA from its first WRITE. Unsafe
state publishes read-only. A runtime sync failure retains its first errno.

Automatic evidence:

- BR-T39 and all eight related existing USB, xHCI, DMA, and SCSI host
  regressions: PASS under `-Wall -Wextra -Werror`;
- final P0/P1 review: no blocker remains;
- `make -j16`, shell syntax checks, and `git diff --check`: PASS;
- legacy BIOS q35/xHCI USB-only root with QEMU write-back cache: PASS through
  `root=overlay` and `login:`, selecting `flush=sync-cache`;
- the same disposable-image gate with `write-cache=off,commandlog=on`: PASS,
  selecting `cache=disabled ... flush=write-through`, with zero
  `SYNCHRONIZE_CACHE` commands in QEMU's SCSI log;
- BR-T24 OVMF q35/xHCI USB root: PASS at 4, 8, and 16 GiB, each through the
  writable overlay and `login:` with RSDP `0x000000007f77e014`;
- BR-T29 xHCI auxiliary-storage remove/re-add: PASS; and
- one final HW-T12 pristine-copy USB-overlay boot: PASS.

`make check` was not run and `.internal/` was not consumed.

Frozen BR-T41 artifact:

- file: [build/amd64/hdd-image-q014.img](../../../build/amd64/hdd-image-q014.img)
- size: 135266304 bytes
- SHA-256:
  `003b54ef77e1fe2e0d96278421441ff7cf4988f736f766f433bf33d6b11cd891`

BR-T41 physical evidence, 2026-08-27:

- The user reports that the requested frozen q014 image completed its one
  planned Latitude cold boot successfully.
- The console photograph shows the boot selector resolving to `/dev/sda1`,
  `loop0` as the private read-only root image, and `loop1` as the private
  read-write data image. Both filesystem checks complete.
- `vfs: root=overlay lower=loop0 upper=loop1`, runtime-filesystem mount, init,
  services, `login:`, root login, `uname`, and an interactive root shell are
  visible. The prior opcode-35/BOT/VFS failure does not recur.
- A second photograph shows X and an interactive root shell in `zterm`. This is
  useful U4 smoke evidence but is not BR-T31 sustained-I/O or BR-T30
  repeatability evidence.
- The cache-policy diagnostic line had already scrolled out of the supplied
  photographs, so its exact `cache`/`dpofua`/`flush` tuple is not invented or
  transcribed. The frozen implementation and BR-T39 fail-closed policy gates,
  together with read-write publication and successful U3, satisfy p010.

Result: BR-T41 **PASS**. Physical U3, p010, and q014 are complete. BR-T30 and
BR-T31 remain later work.

## Interruption / resumption

No p010 work remains. A future regression should be recorded in a new bounded
Phase rather than reopening this completed Queue implicitly. Do not request a
repeat BR-T41 boot; BR-T30 and BR-T31 own later repeatability and sustained-I/O
acceptance.

## Remaining debt and handoff

- SYNCHRONIZE CACHE(16), READ/WRITE(16), UAS, cache-policy administration, and
  power-loss testing remain later storage work.
- BR-T30 still owns final five-consecutive-cold-boot acceptance after U4 is
  otherwise stable. BR-T31 owns sustained physical root I/O and corruption
  checks; p010 establishes only the minimum safe durability path needed for U3.
