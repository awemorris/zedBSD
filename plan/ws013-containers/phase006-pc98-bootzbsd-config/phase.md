# WS013 Phase 006: PC-98 `BOOTZBSD.CFG` boot unification

Last updated: 2026-08-30

Phase ID: `ws013-p006`

Status: Completed (`q032`, 2026-08-30)

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Make the PC-98 `BOOTZBSD.EXE` path load root `/BOOTZBSD.CFG` from its payload
FAT and use exactly the same configuration format, bounds, normalization,
kernel-parameter ABI, root modes, and failure policy as the p003
`zedbsd.cfg` contract.

The different filename is intentional and is the only externally visible
configuration-language difference. Shared behavior does not require shared
loader source:

```text
PC/AT and amd64 UEFI/BIOS: ZEDBSD.CFG
PC-98 BOOTZBSD.EXE:        BOOTZBSD.CFG
```

`BOOTZBSD.CFG` has the FAT 8.3 directory name `BOOTZBSDCFG` and therefore does
not depend on long-filename support merely to locate the configuration.

## Scope

- inventory the native PC-98 LBA2, partition-PBR, DOS/MZ, and
  `BOOTZBSD.EXE` entry paths and select one production `BOOTZBSD.EXE`
  configuration implementation rather than maintaining a fixed-name direct
  loader with different policy;
- read bounded `BOOTZBSD.CFG` from the same FAT as `BOOTZBSD.EXE`;
- consume required `kernel=`, synthesize omitted `boot0` from that FAT's
  serial, apply the p003 shorthand, and pass native `rootpart=` or overlay
  parameters through the existing record;
- load the configured ELF with the existing PC-98 memory/handoff rules;
- update PC-98 image generation and validation to install and verify the
  configuration;
- remove the embedded record and fixed `VMUNIX` fallback from the configured
  production path.

This Phase does not add a PC-98 boot menu, change the PC-98 partition-table
format, or make the UEFI filename visible as a second PC-98 fallback.

An extra q031 preflight reached the current PC-98 kernel and registered its
loader-facing boot device, but reported `physical disks=0` and failed implicit
`boot0` resolution. This is not evidence against the new configuration
grammar, but it is a real baseline prerequisite: p006 must restore physical
disk publication before, or as an explicitly bounded dependency of, its
overlay/native loader acceptance.

## Verification

The reproducible production-loader gate is:

```sh
plan/ws013-containers/tests/run-pc98-zedbsd-config-qemu.sh \
    build/ws013-p006-acceptance
```

- Host/loader fixtures reuse the p003 valid/invalid corpus and prove exact
  parameter-record parity, with only the configuration filename changed.
- PC-98 QEMU boots one overlay configuration and one native-`rootpart`
  configuration through `BOOTZBSD.EXE`.
- Missing/invalid `BOOTZBSD.CFG` and missing/invalid configured kernels stop
  visibly without falling back to fixed `VMUNIX` or embedded parameters.
- The produced PC-98 disk image checker verifies `BOOTZBSD.CFG`,
  `BOOTZBSD.EXE`, configured kernel, and payload images on the intended FAT.
- `make -j16` and the existing PC-98 boot-to-`login:` regression pass.

## Completion conditions

- Every supported PC-98 `BOOTZBSD.EXE` entry path uses the one
  `BOOTZBSD.CFG` contract or is explicitly removed as obsolete.
- The p003 language and parameter result are shared behavior, not a similar
  but independently drifting subset.
- No unconfigured production path silently boots a fixed kernel.

## Reconsideration boundary

Return to planning if the native PC-98 entry path cannot be converged on
`BOOTZBSD.EXE` without changing the established PC-98 boot-media contract, or
if the loader's memory ceiling cannot hold a bounded common parser and
configured kernel-path traversal.

## Actual implementation and review findings

- The installed production image uses the FAT partition PBR to locate and
  enter `BOOTZBSD.EXE`; overlay and native-root acceptance therefore use that
  path. The direct DOS/MZ entry remains in the executable as a legacy launch
  path, but is not used by the production image.
- Configuration and kernel directory entries, FAT12/16/32 successors,
  configured subdirectory chains, file-relative sectors, derived cluster
  LBAs, FAT-sector reads, and ELF file extents are bounded by the mounted FAT
  geometry before disk access. A corrupt directory size cannot make a file
  walk exceed the number of data clusters in the volume.
- The production PBR retains the established contiguous `BOOTZBSD.EXE`
  SYS-file constraint, but no longer trusts an MZ magic and mutable CS:IP
  alone. Before its first loader read, the PBR validates the BPB physical
  volume extent, FAT/root geometry, cluster budget, derived LBA arithmetic,
  and the complete contiguous directory-file extent. Before control transfer
  it validates the fixed MZ envelope, inner ZBL2 version/machine/entry/size
  contract, exact directory-file extent, and checksum over every declared
  ZBL2 sector; Stage 2 repeats that checksum after entry.
- Focused malformed-media acceptance covers an out-of-volume
  `BOOTZBSD.EXE` directory cluster, a reserved first cluster for
  `BOOTZBSD.CFG`, a volume-external kernel successor, and a volume-external
  configured-subdirectory successor. Each stops without reaching the kernel.
  A configured ELF32 entry outside every executable `PT_LOAD` is also rejected
  before segment loading.
- Before fetching any PBR continuation, the first sector now rejects FAT32,
  unsupported bytes-per-sector values, and a FAT16 reserved-area count too
  small to contain the three remaining physical sectors. It also converts and
  bounds the BPB total sector count and rejects an absolute partition start
  whose continuation LBA would wrap. The focused short-reserved, short-total,
  and wrapped-start cells stop before treating FAT/file data as code.
- Final production-PBR QEMU acceptance passed 16/16 cells at
  `build/p006-pc98-pbr-total-final`: overlay, native
  root, a 1024-byte-sector split LFN, four missing/invalid configuration or
  kernel cases, the executable-entry case, and the eight malformed/boundary
  PBR/cluster/chain cases. A separate checksum-corruption cell preserved the
  contiguous FAT chain, flipped one byte in the inner ZBL2 payload, and proved
  that the PBR halted before the Stage 2 debug marker. The 2048-byte PBR
  build, 16-bit helper host test under normal and ASan/UBSan execution, static
  analyzer, and Stage 2 link ceiling (`stage2_end=0x59d1`, below `0x8000`)
  also passed. The PC-98 image builder now preserves the complete disk IPL
  sector byte-for-byte, its checker enforces that identity, and a one-byte
  Stage 1 mismatch was rejected.
- The PC-98 PBR's existing FAT32 branch is not a supported p006 production
  path. Its executable code begins at offset `0x3e`, while a FAT32 BPB extends
  through offset `0x59`; installing a valid FAT32 BPB therefore overwrites the
  PBR code before `BOOTZBSD.EXE` can run. Correcting that on-disk PBR layout is
  a separate WS013 backlog item rather than an expansion of the current
  FAT16/1024-byte-media completion condition. BOOTZBSD's own FAT32 directory
  and file traversal is nevertheless statically range checked.
- The DOS/MZ entry obtains the current DOS drive number, but its current
  native-table path then chooses the first live PC-98 partition. It therefore
  cannot yet guarantee that configuration and kernel come from the same FAT
  as an executable launched from a later DOS partition. Do not infer that
  mapping from table order. A later product decision should either retire the
  direct DOS launch or add an explicit DOS-current-volume-to-native-partition
  resolution/handoff; the production PBR behavior is unchanged here.
