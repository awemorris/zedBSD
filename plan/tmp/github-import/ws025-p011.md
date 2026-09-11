<!-- awesome-plan project=zedbsd record=ws025-p011 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase011/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p011: UFS allocation batch と公開順序

日付: 2026-09-07

Phase ID: `ws025-p011`

Status: completed (q103); see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase011-ufs-allocation-batching/results.md)

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p010、ws025-p014

## 目的と境界

新規作成・追記の CG/super/inode/zero 重複を減らし、初期化と永続化を保証する。

## 変更対象

- `src/drivers/fs/ufs/ufs-vfs.c`
- `src/drivers/fs/ufs/ufs-private.h`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 一 batch を最大 64 KiB・単一 CG・bounded な予約/旧像/新像に限定し、上限や別 CG で区切る。並行 allocator へ未完予約を公開しない。
2. 完全 block は user data で全面初期化して zero を省略可能にする。部分 block/hole は zero を残す。
3. allocation/data の必要な永続化後に private inode/indirect 作業像の pointer/size を公開する。共用 inode を先に汚して別 fsync に流さない。
4. CG/super/inode の重複 update を operation 内でまとめる。abort は公開済み/不確定を返し、pointer clear の永続化を確認できない block を free にしない。
5. journal 前の crash は未到達 allocation の残存を明示し、checker/recovery の対象を定義する。常に fsck 不要とは主張しない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- META04–META07、IO02–IO04。新規/append の各種類の metadata/zero command が baseline より減る。固定の総 command 数は要求しない。
- 各 commit 境界の失敗、CG 跨ぎ、ENOSPC、partial return、rollback 失敗で二重割当てと未初期化参照がない。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

不確定 pointer を解消できなければ保留と書込み停止を記録する。journal をこの Phase 内で新規実装しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。


## q103 concrete execution design

WS024 p001–p004 is now complete. Implement once in the unified owner. Review
at 90 active minutes and continue with recorded facts under autonomous approval.

- Keep the existing inode mutation lock and a bounded allocation context. Select
  at most 64 KiB of file content within one pointer leaf and one allocation CG;
  maintain explicit old/new CG, pointer and dinode state. Smaller or noncontiguous
  opportunities split at a documented boundary rather than wait for memory.
- Preserve the p010 immediate adapter for unsupported/memory-pressure cases while
  making the batched path the normal eligible create/append path. Quota reservation
  must accept a valid prefix when the next block hits a limit, and every pending
  charge must end in commit or rollback.
- Initialize all bytes before publishing pointers: full blocks use user data;
  partial blocks retain zero filling. Persist allocation ownership and initialized
  content before the pointer/size publication stage. Keep the public inode state
  unchanged while constructing private metadata images.
- Serialize shared CG and dinode read/modify/write with the existing mount lock.
  Split lock-owning wrappers from lock-held serialization helpers where needed;
  do not recursively take the mount lock. Preserve inode-before-mount lock order.
- Abort uses explicit publication state. Restore old pointer/dinode bytes and
  confirm the required flush before recycling a possibly referenced block. Failed
  rollback leaves ownership reserved and makes the mount unwritable; it must never
  make an uncertain block available to another allocator.
- First capture create/append counter evidence with existing fixture inputs, then
  extend fault tests around the actual new durability boundaries. Cover direct and
  indirect leaves, CG/slot splits, quota/ENOSPC, partial blocks and failed rollback.
  Run focused host ordinary/sanitizer gates, supported builds and disposable native
  persistence/measurement sequentially. Do not repeat unrelated passed matrices
  without a relevant change or failure.
