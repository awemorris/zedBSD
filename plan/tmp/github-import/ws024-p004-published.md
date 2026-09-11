<!-- awesome-plan project=zedbsd record=ws024-p004 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws024/phase004/phase.md`

親: [ws024](https://github.com/awemorris/zedBSD/issues/25)

# WS024 Phase 004: UFS acceptance and retired-path removal

Last updated: 2026-09-06

Phase ID: `ws024-p004`

Status: completed (q102); see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/phase004-acceptance-and-retirement/results.md)

Parent: [WS024](https://github.com/awemorris/zedBSD/issues/25)

## Objective

Establish end-to-end unified UFS behavior and finish removing the separate
production UFS1/UFS2 implementations and migration scaffolding.

## Work and acceptance

- Exercise high block/file offsets, arithmetic boundaries and invalid input
  on 32-bit and 64-bit ABIs without requiring equally large physical media.
- Retain file/directory/metadata, extended-attribute, quota, journal, snapshot,
  sync/unmount and recovery coverage with production code.
- Run supported configured `make -j16` builds sequentially where outputs are
  shared. Select a finite disposable runtime matrix before execution, covering
  amd64 and maintained i386 PC/AT and PC-98 consumers.
- Verify target formatting, native and overlay root startup, and persistent
  data across unmount/remount and reboot. Compare protected container bytes.
- Remove obsolete active drivers, headers, registrations, format choices,
  builders and temporary aliases/readers once migration acceptance passes.
  Audit active references without erasing historical Queue evidence.

Complete when one UFS production path remains and all selected acceptance
evidence is recorded in P/W/M. Do not use aggregate `make check`, `.internal/`
fixtures, real-media reformatting or an unbounded retry campaign.
