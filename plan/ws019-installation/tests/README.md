# WS019 shared test index

Last updated: 2026-08-29

This directory holds reusable installer and disk-administration fixtures when
the corresponding Phase is implemented. Do not use `.internal/` as a test
source.

| Case | Required result |
| --- | --- |
| `IN-T001` | Read-only administration reports 64-bit disk/partition bounds, GPT type, PARTUUID/PARTLABEL, filesystem, parent, mount/root/swap state, and loader origin from one stable snapshot |
| `IN-T002` | `diskpart list/show` reports the same stable identities and offers no mutation verb |
| `IN-T003` | Preflight accepts exactly one usable ESP plus one explicitly selected distinct same-disk FAT32 and rejects every wrong/ambiguous/aliased case |
| `IN-T004` | Installer publication changes only the six managed paths; GPT, formats, labels, unmanaged sentinels, and UEFI variables remain byte-identical |
| `IN-T005` | Exact existing managed files are idempotent, while any non-identical conflict is refused without overwrite |
| `IN-T006` | Copy, flush, digest, rename, media-change, and interruption failures never report success and remove only unpublished temporary files |
| `IN-T007` | The loader chooses the deterministic first same-disk FAT16/FAT32 with `/zedbsd.cfg`, uses its required `kernel=`, synthesizes omitted `boot0`/bare paths, ignores auxiliary disks, fails on zero, and warns on multiple candidates |
| `IN-T008` | Installed OVMF boot reaches login with `rootfs.img`, writable `data.img`, and active `swapfile`, without installer-created `Boot####` state |
| `IN-T009` | The ordinary single-partition USB source remains bootable and its artifacts are verified before installer use |

GPT writer, raw offsets above 4 GiB, exactly-once raw sector writes, rescan,
formatters, whole-disk confirmation, native-root installation, and destructive
recovery cases belong to future p006/p007 planning and are not installer-v1
acceptance requirements.
