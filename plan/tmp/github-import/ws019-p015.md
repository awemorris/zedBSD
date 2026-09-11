<!-- awesome-plan project=zedbsd record=ws019-p015 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase015/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p015: atomic publication and command durability

Date: 2026-09-09
Status: completed (q130); see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase015-atomic-publication/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

## Scope

既存mvにno-clobberを追加し、check-then-renameを使わずVFS namespace transaction内で宛先不在を判定する。公開UAPIとlibcに明示的renameat2/RENAME_NOREPLACEを追加し、未知flagを拒否、通常renameの互換動作を保持する。FAT directory fsyncをmount metadata flushとdisk barrierに接続する。UNIXで一般的なsyncをuserland/base/へ追加し、全体syncと指定file/directoryのfsyncをエラー伝播付きで扱う。専用installerコマンドは追加しない。

## Acceptance

VFS全mutationの同一transaction参加を確認。ordinary/sanitizerで宛先競合・case alias・same inode・unknown flag・異なるmount・busy objectを検証。QEMUで並行mvと旧rename、directory fsync、failure後の既存ファイル保存を確認。公開後flush失敗は公開済み/永続化未確認として報告し、ファイルを消さない。

Timebox: 120 active minutes per execution queue. Queue投入前に詳細ABI・fixtureを確定する。未完了はunclearedとして他の独立作業へ進む。

## Selected interface

- syscall 163: renameat2(olddirfd, oldpath, newdirfd, newpath, unsigned flags). RENAME_NOREPLACE=1; zero preserves renameat, all other bits EINVAL. Declare in stdio.h and keep existing unistd.h convention too. Shared flag in UAPI, consumed under inode namespace lock before calling driver with zero flags.
- mv: `-n`/`--no-clobber` skip an existing destination without error; `--update=none-fail` diagnoses it and returns failure; `-T`/`--no-target-directory` treats destination as exact path. Installer uses `mv -T --update=none-fail -- STAGE FINAL`. No EXDEV copy fallback is introduced.
- sync: no operands invokes existing sync syscall via libc, clearing/checking errno because this libc's void sync wrapper preserves syscall errors. File operands open O_RDONLY|O_NONBLOCK and call fsync; errors from open/fsync/close propagate. `--` terminates options. No new private command or raw-syscall dependency.
- FAT directory fsync calls existing fat_sync_mount after validating directory inode; its mount mutex owns pending entries, engine flush and disk barrier. Never use regular-file private state for a directory.

References: [renameat2 manual](https://man7.org/linux/man-pages/man2/rename.2.html); installed GNU coreutils mv/sync --help confirms no-clobber versus none-fail and file sync semantics (2026-09-09). GNU online manual fetch timed out; no content was inferred from failed fetch.

Fixture plan: real inode/mount namespace host harness with ordinary/sanitizer, command host fault mocks and real filesystem paths; disposable QEMU FAT + UFS target probe with competing pthread publishers, case aliases, unknown flags, old rename and command exit observers. Build amd64/pcat/pc98 sequentially with explicit configs, no aggregate check.
