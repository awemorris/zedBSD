# ws025-p007: buffer cache の連続 run

日付: 2026-09-07

Phase ID: `ws025-p007`

Status: completed; Queue q094; results.md に証拠を記録

Parent: [WS025](../ws.md)

依存: ws025-p001

## 目的と境界

一回の read/write 要求内の完全 line をまとめ、追加の共有 staging を持たない。

## 変更対象

- `src/kern/buf.c`
- `include/kern/buf.h`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. caller の連続入力/出力を run の転送 buffer とする。部分 line と read hit/miss 境界を分ける。
2. 管理領域準備を multi-line busy 前に行い、run line は try-acquire する。競合時は全解放して bounded retry/短縮する。
3. 成功・部分完了・結果不確定を line ごとに反映する。失敗 dirty を後の sync で回復できる状態に保つ。
4. memory disk と loop 二段で allocation/reclaim/競合再入を注入する。単一 line 互換路の回数と理由を計数する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- IO07–IO10。連続 cold read/full overwrite は device 上限内で一 run。partial/hit/fragment 境界だけで分割する。
- dirty 世代、error、再入、並行 writer、cache cap 縮小の既存契約を維持する。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

busy/reclaim 循環が見つかったら run 取得規則を直してから進む。検証のため cache coherence を無効化しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
