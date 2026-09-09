# ws019-p006: mode selection and whole-disk provisioning

Status: planned; user product decision complete, 2026-09-09
Parent: [WS019](../ws.md)

Offer existing-FAT coexistence or dedicated whole-disk installation early.
Dedicated mode selects a disk and provisions partitions automatically. Retain
a shell action for manual diskpart, but no partition editing screen. Returning
from the shell invalidates all cached disk/partition observations.

Before any write, display the disk identity/capacity and complete-destruction
warning with NO/YES, initially NO. NO/Escape/EOF/input failure make no writes.
YES applies only to the revalidated selected disk; reject source/root/swap,
mounted/claimed and replaced-device aliases. This is product UI confirmation.

Create aligned GPT with FAT32 ESP and current UFS root using remaining space.
Detail ESP sizing from loader/kernel and FAT32 geometry before implementation.
Validate sector arithmetic, small-disk refusal, protective MBR, primary/backup
GPT and reread/reload. Extend existing diskpart/mkfs where provisioning support
is missing. Keep exclusive admission, bounds, flush and error handling.
Failures after initialization starts must report partial destructive state,
never claim restoration of original contents.

Acceptance: both routes and shell return; no-write cancellation/refusal; busy,
source and replaced disk rejection; sector geometry; GPT/FAT32/UFS validation;
write/flush/reload/formatter faults and disposable QEMU integration. No physical
disk writes in automated tests. Split prerequisites and integration into finite
queues. The product-decision block is removed; implementation remains open.

q163 prerequisite: [p030 GPT initialization codec](../phase030-gpt-initialization-codec/phase.md).
p031/q164 completed fd-owned whole-disk admission and p032/q165 connects the
GPT initialization command. Formatting and automatic installer layout follow.
Reject read-only mounts as well as writable mounts, source loop/swap claims,
competing writers and removed media. Do not treat a query as a lock.

Formatter inventory, q165: mkfs currently admits regular files only and
`UFS_FORMAT_MAX_BYTES` is 2147482624. Its initial namespace contains `.zovl0`,
`.zovl1` and `/etc/zedbsd-root`, seeded for overlay data images. Native root
needs a deliberate initial namespace/profile, preserving the accepted coexistence
formatter behavior and the source-tree comparison contract. Extend geometry
and block-device admission; do not silently cap the root partition at 2 GiB
or retain unrequested overlay artifacts to evade a complete tree comparison.
The Noct large-seek fix does not itself remove the formatter's size limit.
q167/q168 completed the portable FAT32 codec and reserved public mkfs command,
including QEMU format/mount/copy and independent mtools validation. Host
zedimage's separate build path still uses mtools. Automatic installer layout
and command orchestration remain open.

q169 adds a native UFS codec with an empty root, persistence tail and 64-bit
fragment/accounting geometry. The historical 2-GiB cap remains only on the
legacy overlay-image profile. Public native mkfs admission and actual QEMU
mount/tree-copy remain prerequisites before p006/p007 acceptance.
