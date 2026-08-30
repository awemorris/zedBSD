# WS013 Phase 006: PC-98 `BOOTZBSD.CFG` boot unification

Last updated: 2026-08-30

Phase ID: `ws013-p006`

Status: planned; not in the current Queue; depends on `ws013-p003`,
`ws013-p004`, and the reusable parser findings from `ws013-p005`

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
