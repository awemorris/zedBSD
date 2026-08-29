# WS019 Phase 003: read-only `/sbin/diskpart`

Last updated: 2026-08-29

Phase ID: `ws019-p003`

Status: planned; depends on `ws019-p002`

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Implement the first `/sbin/diskpart` as a small read-only GPT inspection tool
that gives the installer and user the same stable view of candidate storage.

## Command contract

```text
diskpart list
diskpart show DISK
diskpart show PARTITION
diskpart help
```

No argument is equivalent to `diskpart list`. Output identifies whole disks
and one-based partitions, capacity, filesystem, GPT role/type, PARTUUID,
PARTLABEL, parent disk, and mounted/root/swap/read-only state. Invalid or
ambiguous names fail; there is no edit/create/delete/format verb in this Phase.

## Completion conditions

- Parser/output fixtures cover empty, one-disk, multi-disk, malformed-record,
  media-change, GPT, non-GPT, ESP, FAT32, mounted, and swap cases.
- QEMU output identifies the NVMe disk, its ESP, and payload FAT32 without
  relying on enumeration order.
- Help and diagnostics make the read-only limitation explicit.
- The command is installed to `/sbin/diskpart` by the ordinary base build.

## Reconsideration boundary

Do not add a convenience mutation verb. A request to write GPT returns to the
separately planned destructive administration Phase.
