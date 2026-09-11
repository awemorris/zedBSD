# ws019-p037: shared reserved native UFS command

Status: completed q170; timebox 90 active minutes
Parent: [WS019](../ws.md), prerequisites p035/p036 completed

Expose `mkfs -t ufs --profile=native DEVICE` through the same block admission,
identity confirmation, error reporting and final-close path as FAT32. Rename
the shared command module to reflect both formats. Preserve regular-file UFS
and pristine behavior. Check byte multiplication, codec geometry and native
offset representability before reservation/confirmation. Both paths initially
admit 512-byte sectors, matching the current filesystem mount paths.

Acceptance: both formats' shared lifecycle/grammar/error host tests and old
file frontends; disposable QEMU source/busy refusal and cancellation unchanged;
native format, UFS mount, empty namespace, recursive attribute-preserving copy
with hard links/symlinks, unmount/remount and metadata/byte comparison. Verify
GPT and all unrelated disk ranges unchanged. Exercise a native partition above
2 GiB where practical. FAT32 native regression and three builds pass.

This is public command acceptance, not booted native installation or swap-file
activation; p006/p007 remain responsible for the complete transaction.

q170 acceptance refinement: a writable tmpfs source changes atime while cp
reads it; comparing its post-copy atime to the preserved pre-copy value is
not a valid oracle. Native2 captures before/after/destination stat evidence.
The final fixture prepares a separate UFS source, freezes it read-only before
target formatting, and remounts source/target for the persistent metadata
comparison. The source partition's hash after preparation must remain
unchanged throughout target format/copy/verification. Source preparation is
explicit fixture mutation, outside the target-format no-write baseline.

Accepted 2026-09-09: [results](results.md). Shared host/sanitizer and file
regressions, 4-GiB native QEMU copy/remount, FAT32 regression and three builds pass.
