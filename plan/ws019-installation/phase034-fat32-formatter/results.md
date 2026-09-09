# q167 / ws019-p034 results

Completed 2026-09-09. Added portable userland FAT32 geometry, metadata writer
and exact initial-state verifier in `userland/base/mkfs/fat32-format.c/.h`.
The mkfs package compiles the codec; public command grammar is not yet changed.

| Acceptance | Evidence |
| --- | --- |
| Geometry | 512/1024/2048/4096-byte sectors, checked totals/hidden offsets, FAT capacity, minimum cluster count, upper reserved-cluster range and cluster byte limit |
| Dirty media | Previously nonzero reserved/FAT/root bytes replaced and fully verified |
| Failures | Each write/flush operation fails in turn (41/41/42/45 positions), each read fails in turn (33/33/34/37); short I/O rejects, interrupted read/write retries |
| Publication | Wrapper verifies backup and primary boot records appear only after the required flushes; metadata writes stay within their range |
| Corruption | Primary/backup BPB, primary/backup FSInfo, both FATs and root corruptions rejected |
| Unrelated data | Independent Python reads the entire free data region and compares its known preimage, including first/last sentinel sectors |
| Interoperability | Independent Python BPB/FSInfo/FAT/root decoding and mtools list/create/copy-out exact comparison, all four sector sizes |
| Sanitizers | Ordinary and ASan/UBSan/leak modes pass, `/tmp/zedbsd-q167-fat32-final.log` |
| Production builds | amd64/pcat/pc98 disk-image exit 0, `/tmp/zedbsd-q167-{amd64,pcat,pc98}.log` |

The initial test expected UINT32_MAX sectors to fit every geometry. With
4096-byte sectors and the chosen 32-KiB cluster ceiling this exceeds the
FAT32 cluster limit, so the codec correctly refused it. The test now asserts
that refusal and separately checks a representable large geometry. Initial
failure is retained in `/tmp/zedbsd-q167-fat32.log`; no production workaround
or capacity truncation was introduced.

No live devices were touched. Images were disposable host regular files.
QEMU mount/EFI acceptance requires the next public formatter frontend and
is not claimed here. The kernel FAT driver still supports 512-byte sectors
only. No secure erase, filesystem repair or crash-atomic replacement is
promised. UFS native-root geometry and installer integration remain open.
