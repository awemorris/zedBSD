# q149 managed-file transaction result

Date: 2026-09-09
Phase status: uncleared; file transaction component passes, full installer remains.

Implemented `transaction.noct` and `files.noct`: conflict checks before staging,
all preparations verified before final-name changes, configuration-last atomic
no-replace publication, source/destination revalidation callbacks, final content
checks, directory sync, and cleanup of only positively owned unpublished stages.
A failed rename can be reported as uncertain; no final path is removed during
cleanup. FAT can change st_ino on rename, so final identity is captured again.

The adapter calls existing cp/truncate/mkfs/mkswap/cksum/stat/ls/chmod/mv/sync/rm.
It creates stages from a small private 0755 seed, not executable build modes
that may be 0775. Copies populate already owned stages without preserving an
unsupported source mode. UFS/swap verification uses q148's complete pristine
checks. No private native helper or Noct ioctl was added.

`ls` now refuses a partial enumeration after readdir errors and reports buffered
stdout failure. The existing closedir failure path is retained. This allows
complete listing evidence when distinguishing absence from an uninspectable
existing file; Noct's best-effort listDirectory is not used for that decision.

## Evidence

- `/tmp/zedbsd-q149-transaction-host2.log`: real Noct policy passes 102 scenarios,
  including every operation boundary, failures immediately after create/rename,
  all prior-publication prefixes, exact rerun, conflicting final and unknown stage.
- `/tmp/zedbsd-q149-ls-host3.log`: ordinary and ASan/UBSan directory-read error,
  directory-close error, normal listing and delayed stdout failure PASS.
- `/tmp/zedbsd-q149-{amd64,pcat,pc98}.log`: explicit `make -j16` builds PASS.
  `/tmp/zedbsd-q149-fixture3.log`: final private Noct fixture build PASS.
- `plan/ws019/temp/q149-files2/result.json`: PASS managed-file
  transaction. Target Noct repeats all 102 policy cases. Real commands on fresh
  FAT allocations handle an injected interruption immediately after the third
  actual rename, preserve those final files, clean unpublished stages, resume
  the remaining three files, accept an identical rerun without publication, and
  reject a foreign configuration without modifying it. Generated ZEDSWAP2
  activates/deactivates with 16,383 slots. GPT/FAT boot-label/sentinel and
  production input hashes remain unchanged.

The first native run (`temp/q149-files`) rejected copying an executable's 0775
attributes to FAT. The current seed/mode policy resolves that real assumption
error. It was not a storage corruption or a need to weaken FAT representation.
Host adapter parse caught an unsupported ternary expression; explicit branching
is used. The ls fixture required the real common-command object and time header.

## Exact remaining work

This is a file-transaction component fixture on one disposable FAT partition.
Its loader/kernel/rootfs roles deliberately use a small executable as sample
content. It does **not** produce a bootable installation or exercise selection
of distinct ESP/payload partitions. No public zedinst launcher is installed.

Continue with [admission design](admission-design.md): actual boot provenance,
disk/partition reconciliation, bounded metadata retention, source artifact
validation, capacity, owned mounts/directories, interactive confirmation and
complete packaging. Then run the actual public command and p005 installed
NVMe-only boot. These are implementation tasks, not a hardware or human gate.
