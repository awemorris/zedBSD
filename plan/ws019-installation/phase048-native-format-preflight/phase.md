# ws019-p048: non-mutating formatter admission for native installation

Status: completed q181; see [results](results.md)
Parent: [WS019](../ws.md), p006/p007 prerequisite
Timebox: 90 active minutes

Before whole-disk initialization, validate proposed ESP/UFS sizes using the
same production formatter geometry. Add an explicit non-mutating query mode
to existing mkfs, with strict numeric operands and stable machine output.
Do not create a new installed helper or open any target for mutation. Keep
current reserved device-formatting and regular-file modes unchanged.

Implemented interface: mkfs -t fat32 --check-size BYTES or
mkfs -t ufs --profile=native --check-size BYTES. One newline-terminated,
tab-separated record: format, version 1, fat32/ufs-native, sector size 512,
requested bytes, allocatable full-block bytes, free inodes, allocation unit.
FAT has no fixed inode limit and reports zero in that field. Output failures
are failures. Native available bytes exclude unusable final partial blocks.
Noct admission accounts for cluster-rounded loader/kernel plus EFI/BOOT/config
objects, measured root bytes/8-KiB rounding/swap/headroom, and census objects
plus eight inodes for swap/config replacement/runtime-directory bootstrap.

Export native formatter capacity from its actual computed geometry: data
bytes available after all reserved geometry/persistence tail, and available
inodes. Current native groups have 256 inodes and at most 63936 1024-byte
fragments: byte headroom alone does not prove enough inodes. The q179 pure
planner explicitly still requires this admission, so no destructive caller
may treat its arithmetic result as final authorization.

Connect a Noct preflight wrapper around nativeplan to strict query records.
Check proposed byte counts, sector/profile identity, measured copy size plus
allocation overhead and swap, and conservative inode census including swap
and required installer-created objects. Keep source and target identity
revalidation and exclusive command reservations in the transaction boundary.
Do not claim this query acquires a device lease.

Tests: public query CLI never opens/writes a device; codec-returned capacity
matches independently read formatted superblock/CG summaries; exact and
one-under capacity boundaries, alignment/overflow/profile errors, unsupported
geometry and partial/duplicated Noct records. Retain native and FAT formatter
regressions; build supported architectures. Then connect the complete
native transaction and source/mode/disk UI in p006/p007, followed by source-free
installer acceptance/paging stress and BeUI p029. No human decision blocks this.
