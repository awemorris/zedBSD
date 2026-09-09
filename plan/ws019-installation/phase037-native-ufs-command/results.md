# q170 / ws019-p037 results

Completed 2026-09-09. Public `mkfs -t ufs --profile=native DEVICE` uses the
same block admission/confirmation/close implementation as FAT32. Renamed the
module to `block-command.c/.h`; there is no duplicate native admission path.
The command validates byte multiplication and native geometry before acquiring
the reservation and asking for FORMAT identity confirmation. It retains the
description through metadata write, readback and close. Regular-file UFS
grammar and existing profiles remain unchanged.

| Acceptance | Evidence |
| --- | --- |
| Shared lifecycle | Both formats' independent host syscall/codec oracles pass in ordinary and ASan/UBSan/leak modes; syntax, EOF/wrong identity, failures through final close/output, multiplication overflow and O_RDWR checks |
| Legacy frontends | File-format and pristine UFS/swap regressions pass |
| Native capacity | Public formatter creates/verifies 4294967296 bytes on nvme0n1p2; UFS mount and df succeed |
| Namespace | Initial public find reports only the root, with no overlay files |
| Admission | Source USB and writable/read-only mounted native target refuse; cancel leaves whole test disk unchanged |
| Native copy | cp -a from a separately prepared read-only UFS preserves content, owners, set-id mode, timestamps, symlink and hard-link groups; full diff -r -q --metadata after both remounts passes |
| Source integrity | All bytes outside the target, including the frozen source partition and GPT, retain their post-fixture baseline hash; GPT/prefix/suffix also match the pre-fixture layout baseline |
| FAT32 regression | Public format, mount, copy/execution, remount and mtools byte comparison pass after command sharing |
| Production integrity | Production image unchanged during both successful QEMU runs |
| Builds | amd64/pcat/pc98 disk-image exit 0 |

Native evidence: `temp/q170-native3/{result.json,guest.log,commands.log}`.
FAT32 evidence: `temp/q170-fat32/`.
Logs: `/tmp/zedbsd-q170-command-final.log`, `/tmp/zedbsd-q170-file.log`,
`/tmp/zedbsd-q170-native3.log`, `/tmp/zedbsd-q170-fat32.log`,
`/tmp/zedbsd-q170-{amd64,pcat,pc98}.log`.
Production digest during successful native acceptance:
`65c6c75620ede66c6bfe1509588d8d90c2922528a7838944e5e0d919385ac819`.

Failed experiments are retained. Native1/2 compared a writable tmpfs source's
post-read access time with the copy's correctly preserved pre-read value.
Native2 records source before/after and copied stat: owner 17, group 23,
mode 6751 and modification time matched; source atime advanced while copied
atime retained the original. This was a bad comparison oracle, not a formatter
or cp failure. Native3 prepares a distinct UFS source, freezes its media
read-only before target formatting, and compares persistent metadata using
fresh source/target mounts. The source hash remains unchanged. No timestamp
normalization or relaxed metadata comparator was added.

The initial sandbox host run passed ordinary tests but LeakSanitizer could not
run under ptrace. The final approved unsandboxed sanitizer run passed.

No booted native installation, rootfs.img private-mount integration, native
swap-file activation, installer UI completion or BeUI acceptance is claimed.
These remain the subsequent p006/p007/p029 work.
