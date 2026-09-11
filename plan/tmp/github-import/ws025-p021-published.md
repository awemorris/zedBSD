<!-- awesome-plan project=zedbsd record=ws025-p021 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase021/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p021: 統合 UFS の metadata journal/write-back

日付: 2026-09-07

Phase ID: `ws025-p021`

Status: completed (q116); implementation and final acceptance are recorded in [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase021-ufs-metadata-writeback/results.md).

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p018、ws024-p001–p004

## 目的と境界

単一 64-bit UFS で ordered data + metadata journal/replay を整え、metadata 遅延を限定公開する。

## 変更対象

- `src/drivers/fs/ufs/ (WS024 の予定 owner)`
- `src/kern/vm-object.c`
- `src/kern/disk.c`
- `plan/ws024`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. WS024 の format/driver/image 移行済み成果を取り込む。既存 journal/snapshot の atomicity・checksum・flush・replay 保証を production code で確認する。
2. data-before-pointer、allocation-before-reference、unlink-before-free を transaction/epoch に結び付ける。同じ metadata block に異なる inode の未準備 pointer を混ぜない。
3. commit record と metadata/home block の永続化境界、replay 冪等性、torn-write 検出、log full/checkpoint/backpressure を実装する。
4. quota/xattr/snapshot、directory/rename/truncate、overlay の別 journal との順序を検証する。必要な format 変更は WS024 の版管理に戻し、旧 image を黙って解釈し直さない。
5. opt-in から始める。FAT は同期 batch のまま受け入れ、FAT metadata/mirror の操作間遅延は同等の recovery 根拠がある場合だけ別の有効化項目とする。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- CRASH01–CRASH06、META01–META10、WB01–WB10、FLUSH01–FLUSH06。volatile cache/reorder/torn write を含む各 commit 境界の crash/replay が契約どおり。
- 成功 fsync の対象保持、uncommitted transaction の扱い、replay 後の二重割当てなし、snapshot/quota/xattr/overlay 既存機能を確認する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

journal が不十分なら through/同期 batch に留める。UFS1 専用の新 journal を並行開発しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。


## q111 journal foundation

The sole UFSJ/UFJC codec is version 2, with a ZUJ2 locator and a bounded
multi-extent redo group. User decision: this unreleased OS does not preserve
journal version 1. All producers and mount/recovery use the new format; obsolete
recognized locators are rejected. Journal-profile test images are regenerated.

Durable slot reuse invalidates the old commit before publishing a new descriptor.
Publication, checkpoint and pending reads have explicit sequence/digest ownership;
checkpoint validates all redo before home mutation. Positive committed proof
survives slot retirement so a serialized owner can distinguish a recovered commit
from an abandoned publication. Negative proof alone never authorizes rollback
while I/O remains unresolved. Core crash/replay and existing synchronous feature
tests cover these foundations; they do not establish VFS transaction completion.

Historical q111 remaining work (completed through q116; see final results): concurrent immutable redo read pins;
data-before-pointer/allocation-before-reference/unlink-before-free VFS grouping;
operation-level checkpoint scheduling and bounded pressure; opt-in ownership and fsync/error
epochs; quota/xattr/snapshot/overlay compatibility; CRASH/META/WB/FLUSH and final
supported/native gates. Follow transaction-design.md before enabling metadata delay.
