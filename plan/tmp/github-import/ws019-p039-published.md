<!-- awesome-plan project=zedbsd record=ws019-p039 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase039/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p039: owned Noct root-image source view

Status: completed q172
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Depends on completed p038. Timebox: 90 active minutes.

Generalize workspace attachment with an explicit FAT/UFS type; retain the FAT
wrapper for current callers. Build a shared rootview module resolving the live
loop registration through strict inventory. Admit only read-only parentless
zero-offset geometry matching the retained source bytes. Reobserve root and
registration around mount; check mounted st_dev and owned directory identity.
Register every acquired resource before operations, preserving uncertain
cleanup behavior. Attach after existing backing-file admission and recheck it
in common source preflight, for both installer frontends/modes.

Host JIT/interpreter: valid FAT/UFS lifecycle, unsupported type refusal before
mutation, missing/changed/writable loop, changed root, mount mismatch and cleanup.
Build amd64 image and execute actual Noct shared preflight in disposable QEMU;
verify read-only source and cleanup and source/target hashes. Other architectures
need no rebuild for amd64-only Noct package changes. Full native provisioning,
swap and graphic frontend remain later phases; do not claim full installation.
