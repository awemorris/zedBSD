<!-- awesome-plan project=zedbsd record=ws024-p003 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws024/phase003/phase.md`

親: [ws024](https://github.com/awemorris/zedBSD/issues/25)

# WS024 Phase 003: UFS formatters and image consumers

Last updated: 2026-09-06

Phase ID: `ws024-p003`

Status: completed (q101); legacy fixture dependency retirement and full integration follow in p004

Parent: [WS024](https://github.com/awemorris/zedBSD/issues/25)

## Objective

Make target formatting, maintained image construction and boot consumers use
the same unified UFS format and public name.

## Work and acceptance

- Replace the target UFS1-only encoder with `mkfs -t ufs FILE`; retain the
  completed reservation, identity, size, flush and production read-back gates.
- Update the maintained host Noct backend, Python fixtures and image checkers
  to the selected format. Initialize all required metadata and overlay files.
- Switch platform manifests, native-root markers/probes, rootfs/data image
  builders, package references and boot/mount test consumers together.
  Include `blkid` identification and all six platform manifests/packers.
  Migrate the UFS2 Python builder/checker's UFS1-named base dependencies before
  deleting the old owner. Native root discovery currently contains literal
  `ufs1` type and marker strings, which must follow the format transition.
- Update WS019 formatter/installer contracts and current user documentation;
  retain completed q025/q079 records as history.
- Verify target/host image agreement through the production decoder and
  driver, including prefilled input, refusal and I/O failure cases.

Keep FAT containers, partition tables, swap format and the existing installer
mutation boundary intact. Apply the p001 legacy-image transition explicitly.

Implementation follows [q100 format contract](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/format-contract.md) and [acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/tests/acceptance.md).
