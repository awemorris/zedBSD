# q030 / ws004-p024 NVMe GPT evidence

Date: 2026-08-30 (Asia/Tokyo)
Scope: architecture-independent strict GPT discovery and QEMU NVMe partition
publication. Physical Latitude/NVMe writes remain outside this Phase.

## Host parser and publication gates

The maintained production-source runners passed in ordinary `-Werror`,
ASan/UBSan, and compiler-analyzer modes:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-gpt-host-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/run-partition-publication-test.sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws018-kernel-architecture/tests/run-disklabel-host-test.sh
plan/ws018-kernel-architecture/tests/run-platform-layout-audit.sh
```

Observed results:

- strict GPT: PASS for 512- and 4096-byte logical sectors;
- primary+backup, primary-only, and backup-only validity: PASS;
- contradictory valid headers or entry arrays: rejected;
- bad signature/revision/header-size/header-CRC/table-CRC/reserved fields,
  invalid My/Alternate LBA, unusable geometry, zero disk GUID, and invalid
  entry sizes: rejected;
- header-tail and extended-entry reserved bytes, UEFI attribute bits 3--47,
  and less than 16 KiB of reserved entry-array space: rejected, while
  type-specific attribute bits 48--63 remain accepted;
- an unreadable primary or backup header recovers through the independently
  valid peer after protective-MBR selection;
- one canonical EE plus a compatibility MBR entry: GPT is authoritative;
- any EE or GPT signature followed by invalid GPT: rejected without MBR
  fallback; a pure legacy MBR still falls back to the MBR parser;
- duplicate unique GUIDs, inclusive overlap, out-of-range extents, zero unique
  GUID, and invalid entries beyond caller capacity: rejected;
- excess valid active entries return `-ENOSPC` without partial caller output;
- mixed-endian PARTUUID text, sparse GPT slot indexes, valid surrogate pairs,
  malformed surrogates, and the 108-byte UTF-8 name boundary: PASS; and
- partition publication names index 99 as `nvme0n1p100`, preserves `sda100`
  for a non-numeric parent, and releases allocations after name or
  `disk_create()` failure without releasing an unacquired parent reference.

KA-T010 now proves that the legacy MBR parser does not infer GPT identity.
KA-T011 requires the architecture-neutral `gpt.c`/`pcat-auto.c` owners and the
PC/AT strict GPT/MBR selector.

## Valid GPT QEMU cell

Evidence directory: `build/q030-p024-gpt-final2/valid/`
(disposable/untracked)

- QEMU: 10.0.11 (Debian `1:10.0.11+ds-0+deb13u1`)
- topology: OVMF/q35, disposable IDE system-image copy, one 5-GiB standard PCI
  NVMe namespace initialized by `gpt-image-tool.c`;
- published device used by every guest operation: `/dev/nvme0n1p1`;
- GPT extent: LBA 2048 through 10485726;
- first boot: writes, descriptor `fsync()`, and readback at 8 MiB and above
  4 GiB, 96-command stress, and 4-worker concurrent I/O all passed;
- second QEMU/controller boot: all patterns read back successfully;
- trace cell 1: 2067 I/O commands (252 reads, 1808 writes, 7 flushes),
  SQ wrap 32, CQ wrap 21;
- trace cell 2: 252 reads, SQ wrap 3, CQ wrap 3; and
- source test image before/after SHA-256:
  `736c21df2848daf6716b41985750babdbef3bfe61e84efaaf537491819af0e92`
  (`input_integrity=pass`).

The reusable command is:

```sh
TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/qemu-nvme-gpt.sh OUTPUT-DIRECTORY
```

It extends the p023 I/O runner only through explicit namespace-initializer and
guest-device inputs; the ordinary p023 raw-namespace behavior remains the
default. The same unmodified runner passed its default fresh-zeroed namespace
contract in `build/q030-p024-zeroed/`, so GPT initialization is not an
implicit p023 requirement.

## Malformed GPT QEMU cell

Evidence file:
`build/q030-p024-gpt-final2/broken-guest-logical.log`
(disposable/untracked)

The separate namespace had a canonical protective MBR and intact entry arrays,
but both GPT header CRC fields were damaged. The one-boot rejection cell used
the same QEMU/OVMF topology and observed:

```text
nvme: /dev/nvme0n1 namespace=1 ... writable ...
gpt: nvme0n1 rejected: primary=3 backup=3
login:
```

There was no `vfs: nvme0n1 partition` marker. Thus the controller and raw
namespace were published, strict GPT validation rejected both copies, no
partition child was published or recovered through legacy MBR fallback, and
the independent IDE-root boot continued to login. The source-image SHA-256
remained the value recorded above.

To repeat only this inexpensive negative cell while retaining an already
recorded positive run:

```sh
P024_SKIP_VALID=1 TMPDIR="$PWD/build/q030-tmp" \
  plan/ws004-hardware/tests/qemu-nvme-gpt.sh OUTPUT-DIRECTORY
```

## Build and boot regressions

The final integration passed:

- normal `make -j16`;
- the configured i386 PC/AT kernel build and multiboot contract;
- one amd64 IDE overlay-root boot to `login:`; and
- one amd64 xHCI USB overlay-root boot to `login:`.

The xHCI log also confirms that the 64-bit GPT/VFS diagnostic does not leak an
unsupported `%llu` format token. This document does not claim Latitude
hardware acceptance or any physical-NVMe write.
