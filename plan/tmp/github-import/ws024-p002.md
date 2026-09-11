<!-- awesome-plan project=zedbsd record=ws024-p002 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws024/phase002/phase.md`

親: [ws024](https://github.com/awemorris/zedBSD/issues/25)

# WS024 Phase 002: single 64-bit UFS driver

Last updated: 2026-09-06

Phase ID: `ws024-p002`

Status: completed (q101); final consolidated acceptance and retirement follow in p004

Parent: [WS024](https://github.com/awemorris/zedBSD/issues/25)

## Objective

Implement the p001 contract under one UFS driver owner, using the current
UFS2 codec as the baseline and preserving required behavior from both drivers.

## Work and acceptance

- Consolidate disk decoding, inode and block mapping, allocation, directory
  operations, endian helpers, metadata, sync/unmount and error handling.
- Preserve journal, snapshot, persistent quota and extended-attribute
  functionality under the unified owner; keep overlay journal semantics
  distinct. Retain q077 namespace protection, inode lifetime and rollback.
- Audit address/size narrowing at generic disk, buffer and file boundaries.
  Use checked arithmetic on 32-bit CPUs and document any lower-layer limits.
- Integrate one public registration/header contract with VFS and native-root
  probing. Coordinate removal of obsolete consumers with p003/p004.
- Pass production-linked functional, lifecycle, high-address and malformed
  image fixtures. Temporary migration code must have the p001 removal gate.

This Phase does not introduce an unrelated filesystem feature, generic cache
redesign or new block-device formatter.

Implementation follows [q100 format contract](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/format-contract.md) and [acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/tests/acceptance.md).
