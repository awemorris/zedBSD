<!-- awesome-plan project=zedbsd record=ws024-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws024/phase001/phase.md`

親: [ws024](https://github.com/awemorris/zedBSD/issues/25)

# WS024 Phase 001: unified UFS format and migration contract

Last updated: 2026-09-06

Phase ID: `ws024-p001`

Status: completed; Queue q100; see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/phase001-format-and-migration-contract/results.md)

Parent: [WS024](https://github.com/awemorris/zedBSD/issues/25)

## Objective

Turn the settled single-UFS/64-bit direction into a concrete implementation
and migration contract. Start from the current UFS2 codec and inventory
required behavior from both existing drivers and all image consumers.

## Work and acceptance

1. Freeze disk identification/version, metadata layout, address widths,
   geometry and supported limits, including narrower intermediate arithmetic
   on 32-bit targets and backing-store limits.
2. Inventory native/overlay root behavior, extended attributes, filesystem
   journal/snapshot regions and the distinct overlay journal files. Specify
   their initialization and recovery expectations in the unified format.
3. Specify public `ufs` naming, `mkfs -t ufs FILE`, registration and source
   ownership, and any strictly temporary transition alias.
4. Select rebuild/export/import handling for current UFS1/UFS2 images and
   rejection of unsupported legacy inputs. Record retained-data requirements
   before any conversion implementation; no automatic in-place formatting.
5. Define bounded acceptance fixtures and the producer/consumer switch order,
   so a new driver is not paired with incompatible old build artifacts.

Complete when p002/p003 can implement this contract without reopening the
unification decision or guessing about the handling of existing data.

Frozen implementation contract: [format-contract](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/format-contract.md); [acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/tests/acceptance.md).
