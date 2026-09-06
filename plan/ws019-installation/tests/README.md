# WS019 shared test index

Last updated: 2026-09-06

This directory holds reusable installer and disk-administration fixtures when
the corresponding Phase is implemented. Do not use `.internal/` as a test
source.

| Case | Required result |
| --- | --- |
| `IN-T001` | P002/q076: fixed-width basic block geometry/identity and coherent mount snapshots, malformed ABI, 512/4096 and 64-bit raw I/O, no query writes |
| `IN-T002` | P003: diskpart list/show parses GPT/MBR entirely in userspace, rejects malformed/unsupported tables, reports on-disk vs kernel distinction |
| `IN-T003` | Preflight accepts exactly one usable ESP plus one explicitly selected distinct same-disk FAT32 and rejects every wrong/ambiguous/aliased case |
| `IN-T004` | Installer publication changes only the six managed paths; GPT, formats, labels, unmanaged sentinels, and UEFI variables remain byte-identical |
| `IN-T005` | Exact existing managed files are idempotent, while any non-identical conflict is refused without overwrite |
| `IN-T006` | Copy, flush, digest, rename, media-change, and interruption failures never report success and remove only unpublished temporary files |
| `IN-T007` | The loader chooses the deterministic first same-disk FAT16/FAT32 with `/zedbsd.cfg`, uses its required `kernel=`, synthesizes omitted `boot0`/bare paths, ignores auxiliary disks, fails on zero, and warns on multiple candidates |
| `IN-T008` | Installed OVMF boot reaches login with `rootfs.img`, writable `data.img`, and active `swapfile`, without installer-created `Boot####` state |
| `IN-T009` | The ordinary single-partition USB source remains bootable and its artifacts are verified before installer use |
| `IN-T010` | Target `mkfs -t ufs1 FILE` formats only a pre-sized regular file, matches the production UFS1 format, mounts read/write, and refuses busy/aliased/non-regular targets |
| `IN-T011` | Target `mkswap FILE` formats only a pre-sized page-aligned regular file as ZEDSWAP2, passes the production parser and `swapon`/`swapoff`, and refuses busy/aliased/non-regular targets |
| `IN-T012` | `zedinst` creates unpublished 32-MiB data and 64-MiB swap staging files, invokes the target formatters, and never reads the live source `DATA.IMG` or `SWAPFILE` |
| `IN-T013` | P010: whole-disk EBUSY for any mounted child including ro/root/unchanged, claims/open users, serialized admission, atomic replacement and failure preservation |
| `IN-T014` | P011: userspace GPT/MBR add/delete, confirmed exact target, metadata-only diffs, flush/fault/read-back tests, separate write/reload outcomes |
| `IN-T015` | Q076: disposable QEMU idle reload plus mounted addition rejected with EBUSY, unchanged live mapping, reboot discovers new partitions |
| `IN-T016` | P012: no auxiliary `/diskN` auto-mounts, configured overlay/swap boot intact, explicit ro/rw mounts and reboot regression |

P010/p011 implement reload/existing-table writes independently of the unchanged
non-table-writing installer-v1. Whole-disk initialization, filesystem formatting,
native-root installation and data movement remain future p006/p007 work.

## Q076 execution result

[Evidence, commands and exact residuals](q076-results.md): p002/p003 completed;
p010/p011/p012 implemented but uncleared for final mounted/reboot acceptance.
The fixture now uses `/q076`, because the existing public mount API rejects
nested targets. Do not run another QEMU cell until a new Queue is approved.

Maintained fixtures: `run-storage-foundation-test.sh`,
`run-diskpart-table-test.sh` (parser/writer and production CLI), and
`storage-qemu.mk` / `run-storage-qemu.py` (disposable guest acceptance).

## Q078 formatter verification — uncleared

[Evidence, failed cells and remaining acceptance](q078-results.md) records
p008/p009 implementation and verification. The corrected generators initialize
all metadata and initial allocated content while preserving unused areas;
their ordinary and ASan/UBSan fixtures pass. The first three QEMU launches
stopped before either formatter ran. The fourth and final launch passed all
guest functional checks, including two reboots and persistence. The harness
then failed its production-input hash check because a simultaneous build
regenerated that input; the complete cell is recorded as a harness failure.
Q078 remains finished with p008/p009 uncleared in that cycle. After all correctly
configured builds completed, q079's isolated launch passed and completed both
Phases; its [final result](q079-results.md) is recorded separately.

Run the focused host fixtures from the repository root:

```sh
sh plan/ws019-installation/tests/run-format-reservation-test.sh
sh plan/ws019-installation/tests/run-format-file-test.sh
sh plan/ws019-installation/tests/run-ufs1-format-test.sh
sh plan/ws019-installation/tests/run-swap-format-test.sh
sh plan/ws016-swap-control/tests/run-swap-manager-test.sh
sh plan/ws016-swap-control/tests/run-swap-drain-test.sh
sh plan/ws016-swap-control/tests/run-backing-claim-test.sh
sh plan/ws018-kernel-architecture/tests/run-filesystem-identity-host-test.sh
```

The four formatter runners execute ordinary and ASan/UBSan variants themselves.
UFS1 and swap references are generated afresh through the maintained host Noct
tools; they are independent host comparison fixtures, not templates consumed
by target commands. The reservation test links production file, backing-claim,
VM-object and VM-space code; frontend tests inject descriptor lifecycle faults
without adding production test modes.

Build the supported targets sequentially, because the two i386 platform builds
share image staging paths:

```sh
make -j16 sysroots
make -j16
make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pcat.mk
make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pc98.mk
make -j16 -f Makefile -f plan/ws019-installation/tests/formatter-qemu.mk ws019-formatter-qemu-fixture
```

`formatter-qemu.mk` builds a disposable helper rootfs containing the production
commands plus a waitpid exit observer and file/swap probes. The ordinary image
does not acquire those helpers. `run-formatter-qemu.py` owns fresh output
directories under this WS's ignored `temp/` area and uses
`qemu-system-x86_64`. Consult the current Queue and evidence before a runtime
launch: all four q078 launches and q079's one launch have been consumed. No
aggregate `make check`, production media formatting, or `.internal/` fixtures
are part of this gate.

## Q079 final acceptance

[P008/p009 completed](q079-results.md): one isolated combined QEMU launch
passed target formatter/refusal cases, swap activation/deactivation, generated
overlay/swap boot, two-reboot persistence and unchanged production input plus
protected GPT/FAT/sentinel bytes. All builds finished before this launch.
The finished q079 Queue does not authorize another launch.
