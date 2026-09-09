# WS019 shared test index

Last updated: 2026-09-09

## Interactive confirmation and terminal input (q158)

Run `python3 plan/ws019-installation/tests/installer-confirmation-host.py` from
the repository. It uses the canonical built host Noct in JIT/interpreter modes,
real PTYs, terminal-attribute comparisons, fragmented CSI/UTF-8/SS3, pipe refusal
and truncated-input EOF. It requires no mounted image and makes no disk writes.

After building `transaction-qemu.mk`'s `ws019-transaction-qemu-fixture`, run
`python3 plan/ws019-installation/tests/run-confirmation-qemu.py OUT`, with a fresh
OUT below this WS's temp directory. Eight target cases exercise actual console
Enter, Escape, Ctrl-C and mismatch. It retains protected disk hashes and checks
normal command execution after terminal cleanup. This is component acceptance;
it does not claim public installer integration or an installed NVMe boot.

## Managed-file transaction (q149)

q152 extends the private transaction image with `installer-admission.noct`:
576 manifest/capacity combinations and source/config/record failure boundaries.
Run on host with `noct --path=userland/base/zedinst` in JIT and `-j0` modes.
`run-transaction-qemu.py OUT` retains full transaction recovery; its optional
`--capacity-only` branch exercises actual df admission/refusal without publishing
files. Both use a fresh disposable fixture. See [q152 results](../phase004-zedinst-existing-fat-overlay/q152-results.md).

Capacity prerequisite: `sh plan/ws019-installation/tests/run-df-capacity-host.sh`
runs the real df command against 20,000 independent arithmetic oracle cases
and CLI/statvfs/output faults in ordinary and ASan/UBSan modes. See
[p020/q151](../phase020-df-capacity/results.md).

Run `installer-transaction.noct` with host Noct and
`--path=userland/base/zedinst` for 102 policy/failure scenarios.
`run-ls-completeness-host.sh` checks enumeration/close/output errors in ordinary
and ASan/UBSan modes. Build `transaction-qemu.mk` target
`ws019-transaction-qemu-fixture` with explicit amd64 CI config and `make -j16`,
then run `run-transaction-qemu.py OUT`. Its private FAT component fixture
verifies real-command interrupted publication, recovery, rerun and conflict
refusal. It is not the public installer or an installed boot test.

## Pristine formatter tests

`run-pristine-format-host.sh` checks complete initial UFS (both profiles) and
swap bytes, exact extent arithmetic, corruption and I/O faults against fresh
Noct-generated references. `run-format-file-test.sh` includes both reserved
mutation and read-only frontend syscall/CLI oracles. Both run ordinary and
ASan/UBSan modes. `run-format-reservation-test.sh` now uses
`reservation-current-host.c` with the current full VM/cache owners.

The existing `run-formatter-qemu.py format OUT` additionally executes actual
pristine commands, reversible corruption and SHA-256 non-mutation proofs.
Build its fixture with an explicit amd64 CI config and serialize it with all
production builds/runtime. [q148 evidence](../phase019-pristine-verification/results.md).

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
| `IN-T010` | Target `mkfs -t ufs FILE` formats only a pre-sized regular file, matches the current production 64-bit UFS format, mounts read/write, and refuses busy/aliased/non-regular targets |
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
UFS and swap references are generated afresh through the maintained host Noct
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

Current-format revalidation: [p014](../phase014-current-ufs-formatter/phase.md), q129. Historical q078/q079 outcomes above retain their original scope.

## Current atomic publication (p015/q130)

- `run-publication-vfs-host.py OUT`: actual mount/inode namespace locking,
  concurrent publishers and error preservation, ordinary + ASan/UBSan.
- `run-publication-command-host.sh`: production mv/sync command logic,
  no-clobber/exact path and injected open/fsync/close errors.
- `run-publication-fat-host.py OUT`: current full FAT directory fsync,
  write failure, barrier failure and retry, ordinary + ASan/UBSan.
- `publication-qemu.mk` / `run-publication-qemu.py OUT`: real target syscall,
  mv/sync and reboot persistence on FAT, overlay and native UFS. Disposable
  USB boot/native disks and a single NVMe FAT controller; the initial NVMe
  driver supports one controller, so a second controller is not a fixture.

Use an explicit amd64 config and build the ordinary image, then fixture,
sequentially with make -j16. OUT is a fresh directory under this WS temp.

## Boot-source provenance (p016/q131)

- `run-boot-provenance-host.py OUT`: real loader path decoding, generic kernel
  copy/format, malformed records and V7/legacy envelope classification, ordinary
  and ASan/UBSan.
- `run-boot-provenance-legacy-host.py OUT`: unchanged WS025 memory-handoff and
  WS003 parameter regressions, without the obsolete driver fragment generator.
- `run-boot-provenance-qemu.py MODE OUT`: MODE ordinary, override, ambiguous,
  legacy. Build ordinary amd64 and ws019-storage-qemu-fixture sequentially.
  The legacy case requires the explicitly preserved q131-inputs/BOOTX64-v6.EFI;
  it is a historical fixture, not rebuilt from the new loader source.

All outputs are fresh directories under this WS temp. Each QEMU case records
expected GPT IDs, configuration count and production source hash. The boot0
override case mounts root/data from the auxiliary NVMe while config/kernel
remain sourced from USB. No external media or .internal/ content is used.

## Command staging and capacity (p017/p018)

- `run-staging-cp-host.sh`: actual cp mode, collision, alias and descriptor/error ownership checks.
- `run-growth-fat-host.py OUT`: consolidated FAT12/16/32 capacity and rollback, ordinary and ASan/UBSan.
- Build ordinary amd64, then `staging-qemu.mk` target `ws019-staging-qemu-fixture`, with explicit CI config and make -j16.
- `run-staging-qemu.py OUT`: fresh allocations through target Noct and real commands; output includes source hashes and negative capacity cells.
- `run-staging-reboot-qemu.py STAGING_OUT OUT`: verifies the exact successful staging input hash, boots a disposable copy and checks persistence/swap after reboot.

Run production builds and runtime cells serially. OUT paths must be new directories
under this WS temp. The fixtures never infer allocator acceptance from host
preallocated files and never treat a timed-out mutating command as successful.

## Installer command identity (q134, implementation ongoing)

`run-identity-command-host.py OUT` validates real stat lstat identities and blkid
selection/escaping/query/close/output errors. The maintained diskpart host suite
includes machine records and refusal of machine-mode edits.

Build `ws019-installer-qemu-fixture` using `installer-qemu.mk` after the ordinary
amd64 build, then run `run-installer-command-qemu.py OUT`. This checks the actual
command/Noct identity boundary and unchanged NVMe bytes. It does not perform an
installation and is not p004/p005 acceptance. `installer-identity.noct` also runs
on host Noct with `--path=userland/base/zedinst`.

## Native tree utilities (p028 / q161)

Run find-manifest-test.py, cp-attributes-test.py, cp-tree-test.py,
cp-report-test.py, diff-tree-test.py and tree-copy-test.py for focused host gates.
The Noct parser fixture is installer-treeprogress.noct, in both engines.
run-cp-archive-qemu.py exercises archive metadata/links and the actual Noct
copy monitor. run-cp-time-qemu.py OUT nvme uses a disposable raw NVMe UFS,
seeded nonzero nanoseconds and read-only Noct tree verification; it intentionally
changes one copied permission to assert refusal. These are utility acceptance,
not a dedicated installation. [Final results](../phase028-native-tree-copy-tools/results.md).
