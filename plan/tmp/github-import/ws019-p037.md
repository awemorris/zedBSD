<!-- awesome-plan project=zedbsd record=ws019-p037 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase037/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p037: shared reserved native UFS command

Status: completed q170; timebox 90 active minutes
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20), prerequisites p035/p036 completed

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

Accepted 2026-09-09: [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase037-native-ufs-command/results.md). Shared host/sanitizer and file
regressions, 4-GiB native QEMU copy/remount, FAT32 regression and three builds pass.
