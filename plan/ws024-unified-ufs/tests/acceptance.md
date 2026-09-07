# WS024 bounded acceptance matrix

Frozen by q100 / p001; these are requirements, not passed results.

| ID | Gate |
| --- | --- |
| U01 | Canonical host image decodes with the production unified superblock decoder. |
| U02 | Target mkfs ordinary profile matches host geometry, metadata, root marker and initial overlay journals. |
| U03 | Target/host feature profile initializes valid, disjoint ZUJ2/ZSL1 regions and inactive snapshot state. |
| U04 | Old UFS1 magic/layout and old public CLI/type spelling fail without mutation. |
| U05 | Valid legacy UFS2 codec images mount as UFS; native-root marker transition is explicit. |
| U06 | 32-bit and 64-bit compilation/fixtures map sparse physical fragments above 32-bit address range without narrowing. |
| U07 | Oversized inode size, block count, inode population, shifted sector end and arithmetic overflow fail at the appropriate boundary. |
| U08 | Loop checks original inode size, including values that previously wrapped through uint32_t; existing supported range remains working. |
| U09 | Direct/single/double/triple indirect mapping, holes, fragmentation, overwrite and partial/error return retain data correctness. |
| U10 | Allocation, truncate and shared-dinode-block failure/rollback ownership retain the maintained audit oracles. |
| U11 | Directory create/link/unlink/rename, no-space and rollback preserve namespace identity and open inode lifetime. |
| U12 | Extended attributes round-trip across sync, unmount/remount and malformed/oversized input rejection. |
| U13 | Persistent user/group quota configuration, accounting, limits and system.zedbsd.quota round-trip. |
| U14 | Journal interruption/replay/checksum and failed flush retain recovery behavior without clearing sticky errors. |
| U15 | Snapshot create/read-before-image/delete, full store, failures and unmount-busy rules retain ownership and old content. |
| U16 | Overlay journals and filesystem journal use distinct regions and commit boundaries; temp fsync/rename/directory fsync survives reboot. |
| U17 | Formatter reservation, no-follow identity, fixed-size, refusal, flush/readback and failure tests retain q078/q079 contracts. |
| U18 | amd64, pcat and pc98 supported `make -j16` builds pass with one registration and driver. |
| U19 | QEMU amd64 native UFS root boots from a disposable generated image. |
| U20 | QEMU amd64 ordinary overlay root boots, writes data and verifies it on a second boot. |
| U21 | Maintained ARM64/RPi4 and SPARC producer/check paths generate/check the unified profile; do not claim unexecuted physical boots. |
| U22 | Source/symbol/manifest inventory finds no second production UFS1/UFS2 driver/formatter codec, and active fixtures have valid source dependencies. |
| U23 | WS025 pool/runs/metadata-view/logical-epoch/BIO-frontier regressions retain their contracts after consolidation. |
| U24 | Ordinary aligned native I/O baseline records both workload modes, readback, counts and latency; explain format/journal profile when comparing with pre-migration results. |

Host fixtures use ordinary and ASan/UBSan variants where supported. High-address
fixtures use sparse synthetic storage; this is not a claim of a multi-terabyte
real-device test. Physical gate remains user-accepted under the WS025 instruction.
Generated source images are backed up and runtime tests use disposable copies.
