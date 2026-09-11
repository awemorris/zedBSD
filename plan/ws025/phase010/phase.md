# ws025-p010: UFS allocation adapter と metadata cache

日付: 2026-09-07

Phase ID: `ws025-p010`

Status: completed; Queue q097; see [results](results.md)

Parent: [WS025](../ws.md)

依存: ws025-p008

## 目的と境界

意味を変えずに allocation の境界を導入し、CG/indirect の再読込みを減らす。

## 変更対象

- `src/drivers/fs/ufs1/ufs1-vfs.c`
- `src/drivers/fs/ufs2`
- `src/kern/buf.c` / `include/kern/buf.h`
- `src/kern/disk.c` / `include/kern/disk.h`（cache view の lifecycle admission owner）

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. alloc begin/allocate/commit/abort を既存経路に接続し、まだ保留 metadata を持たない互換段階を実装する。commit no-op はこの段階だけとする。
2. CG の valid bit、mount/media/content 世代、dirty owner を導入し、番号ゼロの初期値を hit としない。
3. indirect cache は共通予算の bounded cache/pin とし、inode ごとに無条件の 8 KiB を持たない。
4. truncate、block reuse、rollback、CG switch、別操作による更新の無効化を整える。production owner の UFS 名は WS024 の進捗と同期する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- META01–META03。adapter 前後で allocation/publication/返値と error が一致し、counter は初段の期待値を維持する。
- 96 KiB 超等の indirect read と CG 反復は cache hit を確認し、stale entry は一切採用しない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

共有 CG 作業像の所有者が曖昧なら batching を有効化せず、p011 の前提へ戻す。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
