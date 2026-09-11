# WS013 Phase 002: UEFI `zedbsd.cfg` volume discovery

Last updated: 2026-08-30

Phase ID: `ws013-p002`

Status: completed in `q031`

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Teach `BOOTX64.EFI` to inspect FAT16/FAT32 filesystems on the same physical
disk as its loaded image, select the first one containing a readable root
`/zedbsd.cfg`, and make that selected volume—not necessarily the loader's
own filesystem—the source of the UEFI configuration, kernel, and implicit
`boot0` identity.

## Dependencies

- the q015 textual boot-parameter and loader-origin FAT UUID handoff contract;
- UEFI Loaded Image, Device Path, Simple File System, and Block I/O protocols;
- no GPT-only identity requirement and no dependency on UEFI NVRAM mutation.

## Same-disk boundary

The loaded image's `DeviceHandle` establishes the physical boot disk. The
implementation compares bounded UEFI device paths through the physical-device
portion and treats partition/media-file nodes as children of that disk.
Candidates whose device path is malformed, truncated, unsupported, or cannot
be proven to share that physical parent are excluded with a diagnostic.
Configuration on an auxiliary disk is never a fallback.

The comparison must support the GPT and MBR hard-drive device paths already
used by supported UEFI media. Partition numbers, GPT names, partition GUIDs,
FAT labels, and the ESP attribute are not selection criteria. The
configuration is allowed to reside on the filesystem from which
`BOOTX64.EFI` was loaded.

## Ordered selection contract

1. Obtain the loaded image filesystem and physical-disk device-path identity.
2. Form one ordered scan:
   - inspect the loaded image filesystem first;
   - then inspect SimpleFS handles in the exact order returned by firmware;
   - skip the loaded handle when it reappears and de-duplicate any repeated
     handle.
3. Retain only logical-partition Block I/O handles on the same physical disk.
   Validate their on-media BPB as FAT16 or FAT32; SimpleFS presence alone is
   not proof of a supported FAT. FAT12, exFAT, non-FAT, absent media, and
   malformed BPBs are not candidates.

The accepted media geometry deliberately matches the kernel FAT driver:
UEFI Block I/O uses 512-byte media blocks, while the FAT BPB may declare
512- or 1024-byte logical sectors. Capacity comparison accounts for that
ratio. Other media block sizes and BPB sector sizes are rejected so a volume
selected by the loader cannot later be rejected solely by the kernel's disk
contract.
4. Open each supported volume and test for a readable root
   `/zedbsd.cfg`. Record the first match while continuing the bounded scan
   so later matches can be diagnosed.
5. If there is no match, print that `zedbsd.cfg` was not found on the boot
   disk and stop. There is no embedded-parameter, fixed-kernel, loader-volume,
   or cross-disk fallback.
6. If there is more than one match, print a warning including the bounded
   match count and the selected candidate, then continue with the first match
   from step 2. Do not sort by partition number, GUID, label, or filename.
7. Hand the selected root/file handle and FAT volume serial to p003. If p003
   later finds an invalid configuration or kernel, fail on that first
   candidate; do not retry a later matching volume.

“First” is therefore testable: loader filesystem first, then firmware
enumeration order. The warning preserves convenience for novice-created media
while keeping the chosen volume explainable.

## Selected-volume identity

Read and validate the FAT16/FAT32 extended BPB volume serial from the selected
candidate. Format it using the kernel's canonical FAT UUID representation
(`XXXX-XXXX`) and preserve it as the selected-volume identity used by p003
when `boot0=` is omitted. The handoff's loader-origin FAT identity must also
describe the selected configuration volume rather than blindly retaining the
filesystem that contained `BOOTX64.EFI`.

An explicit `boot0=` in `zedbsd.cfg` remains p003 input and suppresses
parameter-string synthesis. p002 does not inspect root mode, swap choices, or
the `kernel=` value while deciding which marker wins.

## Validation and cleanup

- Bound the number and total byte length of firmware handles and device-path
  nodes before walking them.
- Distinguish `EFI_NOT_FOUND` from media/open failures in diagnostics, but
  keep selection based only on successful readable markers.
- Continue after a later-candidate error once the first candidate is retained
  so the final warning/count remains deterministic; never replace the first
  candidate with a later “better” configuration.
- Close every non-selected root and marker handle immediately. Close the
  selected configuration/root handles at their p003 ownership boundary.
- Free every handle buffer, device-path buffer, and pool allocation on both
  success and all pre-`ExitBootServices()` failures.
- Do not write a filesystem, GPT, UEFI variable, or boot option.

## Compatibility

This Phase changes only the amd64 UEFI path. Legacy amd64 BIOS, i386 PC/AT,
and PC-98 keep their current boot-source behavior. Existing single-FAT UEFI
media remain usable only after they contain `/zedbsd.cfg`; absence is the
same visible fatal error as on multi-partition media.

UEFI `LoadOptions` handling and configuration parsing are p003 concerns.
They do not influence which FAT contains the winning `zedbsd.cfg`.

## Completion conditions

- Host fixtures cover loaded-filesystem-first order, firmware enumeration
  order, handle de-duplication, same-disk comparison for GPT and MBR paths,
  malformed/truncated device paths, and bounded cleanup.
- FAT16 and FAT32 candidates pass; FAT12, exFAT, non-FAT, malformed BPB,
  missing media, and unreadable-root/config candidates do not.
- A zero-match OVMF cell prints a visible not-found error and stops even when
  an auxiliary disk contains `zedbsd.cfg`.
- Multiple-match cells print a warning and boot the loaded-filesystem match
  first, or the first same-disk firmware-enumerated match when the loaded
  filesystem has no marker.
- A same-disk configuration FAT distinct from the ESP supplies the selected
  FAT UUID/loader-origin identity, and a cross-disk marker is ignored.
- Every tested failure before `ExitBootServices()` closes files and roots and
  frees firmware allocations without changing on-disk state.

## Actual results (2026-08-30)

- `run-uefi-volume-discovery-test.sh` passed its ordinary, sanitizer, and GCC
  analyzer variants, including bounded device paths, GPT/MBR same-disk
  comparison, FAT16/FAT32 BPBs, ordered handle de-duplication, match counts,
  selected UUID formatting, and cleanup ownership.
- A final independent review found and closed one loader/kernel geometry
  mismatch. The post-review fixture accepts 512-byte Block I/O with either a
  512- or 1024-byte FAT logical sector, checks the 2:1 capacity boundary, and
  rejects non-512 media and unsupported BPB sector sizes; ordinary,
  ASan/UBSan, and analyzer variants pass.
- `run-uefi-zedbsd-config-ovmf.sh` passed 7/7 q35/OVMF cells in
  `build/q031-ws013-ovmf-final`. The loaded-FAT cell selected order 0 and
  `UUID=1111-2222`; the separate same-disk FAT cell logged missing order 0,
  selected order 1, and used `UUID=3333-4444`.
- The zero-match cell first caused firmware to connect a non-bootable
  auxiliary FAT, then loaded `BOOTX64.EFI` from the real boot disk. Its log
  contained `A64 CFG OTHER DISK 0x0000000000000001`, the boot-disk not-found
  diagnostic, and no selected volume, kernel, ELF, or kernel parameter marker
  during a two-second post-error observation interval.
- The duplicate cell logged matches at orders 0 and 1, match count 2, the
  visible multiple-config warning, and selected the loaded FAT at order 0.
  The invalid selected-config cell retained that same selection and stopped;
  the valid second configuration was not a retry target.
- Every QEMU disk was newly constructed below the result directory and
  attached read-only. The per-cell debug-console, serial, QEMU-command, media
  layout/hash, and result files retain the exact discovery evidence.
- The complete seven-cell OVMF matrix passed again after the review repair in
  `build/q031-root-ovmf-final-review`.

## Reconsideration boundary

Return to planning if supported firmware does not expose enough device-path
identity to prove same-physical-disk membership, if its SimpleFS enumeration
cannot be consumed within a fixed bound, or if the selected FAT cannot be
represented through the existing UUID/loader-origin handoff without adding a
new ABI.
