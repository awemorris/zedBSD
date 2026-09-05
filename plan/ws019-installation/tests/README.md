# WS019 shared test index

Last updated: 2026-09-05

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
